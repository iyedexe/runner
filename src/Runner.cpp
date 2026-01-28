#include "Runner.h"
#include "codegen/fix/OE/FixValues.h"
#include "logger.hpp"
#include "crypto/utils.hpp"

Runner::Runner(const RunnerConfig& config)
    : config_(config)
{
    LOG_INFO("[Runner] Loading ED25519 key from: {}", config.ed25519KeyPath);
    key_ = std::make_unique<crypto::ed25519>(readPemFile(config.ed25519KeyPath));

    LOG_INFO("[Runner] Creating Admin (REST client) for: {}", config.restEndpoint);
    admin_ = std::make_unique<Admin>(config.restEndpoint, config.apiKey, *key_);

    LOG_INFO("[Runner] Creating Feeder (FIX market data)");
    feeder_ = std::make_unique<Feeder>(config.apiKey, *key_);

    LOG_INFO("[Runner] Creating Broker (FIX order execution, liveMode={})", config.liveMode);
    broker_ = std::make_unique<Broker>(config.apiKey, *key_, config.liveMode);

    LOG_INFO("[Runner] Creating TriangularArbitrage strategy");
    strategy_ = std::make_unique<TriangularArbitrage>(config.strategyConfig);
}

void Runner::initialize() {
    LOG_INFO("[Runner] Initializing...");

    symbolsList_ = admin_->fetchExchangeInfo();

    orderSizer_.clear();
    for (const auto& symbol : symbolsList_) {
        orderSizer_.addSymbol(symbol.to_str(), symbol.getFilters());
    }

    strategy_->discoverRoutes(symbolsList_);

    balance_ = admin_->fetchAccountBalances();

    const auto& startingAsset = strategy_->startingAsset();
    if (balance_.find(startingAsset) == balance_.end()) {
        LOG_WARNING("[Runner] No balance found for starting asset: {}", startingAsset);
        balance_[startingAsset] = 0.0;
    } else {
        LOG_INFO("[Runner] Starting asset {} balance: {}", startingAsset, balance_[startingAsset]);
    }

    LOG_INFO("[Runner] Connecting FIX sessions...");
    feeder_->connect();
    broker_->connect();

    LOG_INFO("[Runner] Waiting for FIX logon...");
    feeder_->waitUntilConnected();
    broker_->waitUntilConnected();

    LOG_INFO("[Runner] FIX sessions connected");

    if (!symbolsList_.empty()) {
        std::vector<std::string> symbolsAsStrings;
        symbolsAsStrings.reserve(symbolsList_.size());
        for (const auto& s : symbolsList_) {
            symbolsAsStrings.push_back(s.to_str());
        }
        
        LOG_INFO("[Runner] Subscribing to market data for {} symbols", symbolsAsStrings.size());
        feeder_->subscribeToSymbols(symbolsAsStrings);

        // Wait for all market data snapshots to arrive before starting
        waitForMarketDataSnapshots();
    }

    LOG_INFO("[Runner] Initialization complete");
}

void Runner::shutdown() {
    LOG_INFO("[Runner] Shutting down...");

    if (feeder_) {
        feeder_->disconnect();
    }
    if (broker_) {
        broker_->disconnect();
    }
}

void Runner::waitForMarketDataSnapshots() {
    LOG_INFO("[Runner] Waiting for market data snapshots...");

    // Wait up to 30 seconds for all snapshots
    bool success = feeder_->waitForAllSnapshots(30000);

    auto [received, expected] = feeder_->getSnapshotProgress();
    if (success) {
        LOG_INFO("[Runner] All market data snapshots received ({}/{})", received, expected);
    } else {
        LOG_WARNING("[Runner] Timeout waiting for snapshots, received {}/{}", received, expected);
    }
}

void Runner::executeArbitrage(const Signal& signal) {
    const auto& startingAsset = strategy_->startingAsset();

    LOG_INFO("[Runner] ========== EXECUTING ARBITRAGE ==========");
    LOG_INFO("[Runner] Mode: {}", config_.liveMode ? "LIVE" : "TEST");
    LOG_INFO("[Runner] Path: {}", signal.description);
    LOG_INFO("[Runner] Theoretical PnL: {:.8f}", signal.pnl);
    LOG_INFO("[Runner] {} Balance: {:.8f}", startingAsset, balance_[startingAsset]);

    // Track execution results for PnL calculation
    struct LegResult {
        std::string symbol;
        Way way;
        double estPrice;
        double realPrice;
        double estQty;
        double realQty;
        double feeRate;
    };
    std::vector<LegResult> results;
    results.reserve(signal.orders.size());

    size_t legIndex = 0;

    // Orders are already validated by evaluate() - just execute them sequentially
    for (const auto& order : signal.orders) {
        char side = (order.getWay() == Way::BUY) ? FIX::OE::Side_BUY : FIX::OE::Side_SELL;
        std::string symbol = order.getSymbol().to_str();
        double qty = order.getQty();
        double estPrice = order.getPrice();
        double feeRate = strategy_->getFeeForSymbol(symbol) / 100.0;

        LOG_INFO("[Runner] Leg {}: {} {} @ MARKET, estPrice={:.8f}, qty={:.8f}",
                 legIndex + 1, (side == FIX::OE::Side_BUY ? "BUY" : "SELL"), symbol, estPrice, qty);

        std::string clOrdId;
        clOrdId = broker_->sendMarketOrder(symbol, side, qty, estPrice);

        // Wait for order completion
        auto status = broker_->waitForOrderCompletion(clOrdId, 5000);

        // Check for rejection or timeout
        if (status == OrderStatus::REJECTED) {
            auto orderState = broker_->getOrderState(clOrdId);
            LOG_CRITICAL("[Runner] Leg {}: Order {} REJECTED: {}",
                        legIndex + 1, clOrdId, orderState.rejectReason);
            balance_ = admin_->fetchAccountBalances();  // Sync balances
            throw ArbitrageExecutionError(
                "Order rejected at leg " + std::to_string(legIndex + 1) + ": " + orderState.rejectReason,
                static_cast<int>(legIndex), clOrdId);
        }

        if (status == OrderStatus::UNKNOWN) {
            LOG_CRITICAL("[Runner] Leg {}: Order {} TIMEOUT - status unknown",
                        legIndex + 1, clOrdId);
            balance_ = admin_->fetchAccountBalances();  // Sync balances
            throw ArbitrageExecutionError(
                "Order timeout at leg " + std::to_string(legIndex + 1) + " - manual intervention required",
                static_cast<int>(legIndex), clOrdId);
        }

        if (status != OrderStatus::FILLED) {
            LOG_CRITICAL("[Runner] Leg {}: Order {} unexpected status={}",
                        legIndex + 1, clOrdId, static_cast<int>(status));
            balance_ = admin_->fetchAccountBalances();  // Sync balances
            throw ArbitrageExecutionError(
                "Order failed at leg " + std::to_string(legIndex + 1) + " with status " + std::to_string(static_cast<int>(status)),
                static_cast<int>(legIndex), clOrdId);
        }

        auto orderState = broker_->getOrderState(clOrdId);
        double realPrice = orderState.avgPx;
        double realQty = orderState.cumQty;

        // Check for partial fill (should not happen with FILLED status, but be safe)
        if (realQty < qty * 0.99) {  // Allow 1% tolerance for rounding
            LOG_CRITICAL("[Runner] Leg {}: Order {} PARTIAL FILL: requested={:.8f}, filled={:.8f}",
                        legIndex + 1, clOrdId, qty, realQty);
            balance_ = admin_->fetchAccountBalances();  // Sync balances
            throw ArbitrageExecutionError(
                "Partial fill at leg " + std::to_string(legIndex + 1) + ": requested " +
                std::to_string(qty) + ", filled " + std::to_string(realQty),
                static_cast<int>(legIndex), clOrdId);
        }

        // Calculate slippage (guard against division by zero)
        double slippage = (estPrice > 0) ? ((realPrice - estPrice) / estPrice * 100.0) : 0.0;

        LOG_INFO("[Runner] Leg {}: FILLED clOrdId={}", legIndex + 1, clOrdId);
        LOG_INFO("[Runner]   Est  Price: {:.8f} | Real Price: {:.8f} | Slippage: {:+.4f}%",
                 estPrice, realPrice, slippage);
        LOG_INFO("[Runner]   Est  Qty:   {:.8f} | Real Qty:   {:.8f}",
                 qty, realQty);

        results.push_back({symbol, order.getWay(), estPrice, realPrice, qty, realQty, feeRate});
        ++legIndex;
    }

    // All legs executed successfully - calculate and report PnL
    {
        double balanceBefore = balance_[startingAsset];

        // Refresh balances from exchange
        balance_ = admin_->fetchAccountBalances();
        double balanceAfter = balance_[startingAsset];
        double actualPnl = balanceAfter - balanceBefore;

        // Trace through actual execution to calculate expected final amount
        // Use REAL executed quantities and prices
        double traceAmount = results[0].realQty;
        if (results[0].way == Way::BUY) {
            // First leg was BUY: we spent quote to get base
            // Input was quote = realQty * realPrice
            traceAmount = results[0].realQty * results[0].realPrice;
        }
        double initialStake = traceAmount;

        for (const auto& r : results) {
            if (r.way == Way::BUY) {
                // BUY BASE/QUOTE: input quote, output base = (quote / price) * (1 - fee)
                traceAmount = (traceAmount / r.realPrice) * (1.0 - r.feeRate);
            } else {
                // SELL BASE/QUOTE: input base, output quote = (base * price) * (1 - fee)
                traceAmount = (traceAmount * r.realPrice) * (1.0 - r.feeRate);
            }
        }

        double tracedPnl = traceAmount - initialStake;
        double tracedPnlPct = (initialStake > 0) ? (tracedPnl / initialStake * 100.0) : 0.0;
        double actualPnlPct = (initialStake > 0) ? (actualPnl / initialStake * 100.0) : 0.0;

        LOG_INFO("[Runner] ========== EXECUTION SUMMARY ==========");
        LOG_INFO("[Runner] {} Balance Before: {:.8f}", startingAsset, balanceBefore);
        LOG_INFO("[Runner] {} Balance After:  {:.8f}", startingAsset, balanceAfter);
        LOG_INFO("[Runner] Actual PnL:        {:.8f} ({:+.4f}%)", actualPnl, actualPnlPct);
        LOG_INFO("[Runner] Traced PnL:        {:.8f} ({:+.4f}%)", tracedPnl, tracedPnlPct);
        LOG_INFO("[Runner] Theoretical PnL:   {:.8f}", signal.pnl);
        LOG_INFO("[Runner] ========================================");
    }
}

void Runner::run() {
    LOG_INFO("[Runner] Starting main loop...");

    const auto& startingAsset = strategy_->startingAsset();
    const double risk = strategy_->risk();

    while (true) {
        try {
            auto updatedBooks = feeder_->waitForBookUpdate();

            auto balanceIt = balance_.find(startingAsset);
            if (balanceIt == balance_.end() || balanceIt->second <= 0) {
                return;
            }
            const double stake = risk * balanceIt->second;

            std::optional<Signal> sig = strategy_->onMarketDataBatch(updatedBooks, stake, orderSizer_);

            if (sig.has_value()) {
                executeArbitrage(*sig);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[Runner] Error in main loop: {}", e.what());
            break;
        }
    }
}

RunnerConfig Runner::loadConfig(const std::string& configFile) {
    RunnerConfig config;
    boost::property_tree::ptree pt;

    try {
        boost::property_tree::ini_parser::read_ini(configFile, pt);

        // Strategy config
        config.strategyConfig.startingAsset = pt.get<std::string>("TRIANGULAR_ARB_STRATEGY.startingAsset");
        config.strategyConfig.defaultFee = pt.get<double>("TRIANGULAR_ARB_STRATEGY.defaultFee", 0.1);
        config.strategyConfig.risk = pt.get<double>("TRIANGULAR_ARB_STRATEGY.risk", 1.0);

        // Runner config
        config.liveMode = pt.get<bool>("TRIANGULAR_ARB_STRATEGY.liveMode", false);
        config.fixMdEndpoint = pt.get<std::string>("FIX_CONNECTION.mdEndpoint", "fix-md.testnet.binance.vision");
        config.fixMdPort = pt.get<int>("FIX_CONNECTION.mdPort", 9000);
        config.fixOeEndpoint = pt.get<std::string>("FIX_CONNECTION.oeEndpoint", "fix-oe.testnet.binance.vision");
        config.fixOePort = pt.get<int>("FIX_CONNECTION.oePort", 9000);
        config.restEndpoint = pt.get<std::string>("FIX_CONNECTION.restEndpoint", "testnet.binance.vision");
        config.apiKey = pt.get<std::string>("FIX_CONNECTION.apiKey");
        config.ed25519KeyPath = pt.get<std::string>("FIX_CONNECTION.ed25519KeyPath");

        // Parse per-symbol fees from [SYMBOL_FEES] section
        auto symbolFeesSection = pt.get_child_optional("SYMBOL_FEES");
        if (symbolFeesSection) {
            for (const auto& item : *symbolFeesSection) {
                config.strategyConfig.symbolFees[item.first] = item.second.get_value<double>();
            }
        }

    } catch (const boost::property_tree::ini_parser_error& e) {
        throw std::runtime_error("Failed to load config file: " + std::string(e.what()));
    } catch (const boost::property_tree::ptree_bad_path& e) {
        throw std::runtime_error("Missing parameter in config file: " + std::string(e.what()));
    }

    return config;
}
