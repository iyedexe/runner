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
    feeder_ = std::make_unique<Feeder>(config.apiKey, *key_, orderBook_);

    LOG_INFO("[Runner] Creating Broker (FIX order execution, liveMode={})", config.liveMode);
    broker_ = std::make_unique<Broker>(config.apiKey, *key_, config.liveMode);

    LOG_INFO("[Runner] Creating TriangularArbitrage strategy");
    strategy_ = std::make_unique<TriangularArbitrage>(config.strategyConfig);

    LOG_INFO("[Runner] Creating TradePersistence in: {}", config.tradeLogDir);
    tradePersistence_ = std::make_unique<TradePersistence>(config.tradeLogDir);
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

    // Subscribe only to symbols that are part of arbitrage paths
    const auto& strategySymbols = strategy_->subscribedSymbols();
    if (!strategySymbols.empty()) {
        std::vector<std::string> symbolsToSubscribe(strategySymbols.begin(), strategySymbols.end());

        LOG_INFO("[Runner] Subscribing to market data for {} symbols (out of {} total)",
                 symbolsToSubscribe.size(), symbolsList_.size());
        feeder_->subscribeToSymbols(symbolsToSubscribe);

        waitForMarketDataSnapshots();
    } else {
        LOG_WARNING("[Runner] No arbitrage paths found, no symbols to subscribe to");
    }

    LOG_INFO("[Runner] Initialization complete");
    LOG_INFO("[Runner] Polling mode: {}",
             config_.pollingMode == PollingMode::Blocking ? "Blocking" :
             config_.pollingMode == PollingMode::BusyPoll ? "BusyPoll" : "Hybrid");
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

    bool success = feeder_->waitForAllSnapshots(30000);

    auto [received, expected] = feeder_->getSnapshotProgress();
    if (success) {
        LOG_INFO("[Runner] All market data snapshots received ({}/{})", received, expected);
    } else {
        LOG_WARNING("[Runner] Timeout waiting for snapshots, received {}/{}", received, expected);
    }
}

void Runner::handleExecutionFailure(int legIndex, const std::string& clOrdId, const std::string& reason,
                                    const std::vector<ExecutedOrder>& executedOrders) {
    LOG_CRITICAL("[Runner] ========== EXECUTION FAILURE ==========");
    LOG_CRITICAL("[Runner] Failed at leg {}: {}", legIndex + 1, reason);

    // Execute rollback for previously successful orders
    if (!executedOrders.empty()) {
        LOG_WARNING("[Runner] Initiating rollback for {} executed order(s)", executedOrders.size());
        bool rollbackSuccess = executeRollback(executedOrders);
        if (rollbackSuccess) {
            LOG_INFO("[Runner] Rollback completed successfully");
        } else {
            LOG_CRITICAL("[Runner] ROLLBACK PARTIALLY FAILED - manual intervention required");
        }
    } else {
        LOG_INFO("[Runner] No orders to rollback (failed on first leg)");
    }

    // Refresh balance after rollback attempts
    balance_ = admin_->fetchAccountBalances();

    LOG_CRITICAL("[Runner] ==========================================");
    throw ArbitrageExecutionError(reason, legIndex, clOrdId);
}

bool Runner::executeRollback(const std::vector<ExecutedOrder>& executedOrders) {
    LOG_WARNING("[Runner] ========== EXECUTING ROLLBACK ==========");

    bool allRollbacksSucceeded = true;
    constexpr int ROLLBACK_TIMEOUT_MS = 10000;  // Longer timeout for rollback orders
    constexpr int MAX_ROLLBACK_RETRIES = 1;     // Only retry once to avoid infinite loops

    // Process rollbacks in reverse order (LIFO) to properly unwind the position
    for (auto it = executedOrders.rbegin(); it != executedOrders.rend(); ++it) {
        const auto& executed = *it;

        // Determine opposite side: BUY ('1') -> SELL ('2'), SELL ('2') -> BUY ('1')
        char rollbackSide = (executed.side == FIX::OE::Side_BUY) ? FIX::OE::Side_SELL : FIX::OE::Side_BUY;
        const char* sideStr = (rollbackSide == FIX::OE::Side_BUY) ? "BUY" : "SELL";
        const char* origSideStr = (executed.side == FIX::OE::Side_BUY) ? "BUY" : "SELL";

        LOG_WARNING("[Runner] Rollback: {} {} qty={:.8f} (original was {} @ {:.8f})",
                    sideStr, executed.symbol, executed.filledQty,
                    origSideStr, executed.avgPrice);

        bool rollbackSucceeded = false;
        int retryCount = 0;

        while (!rollbackSucceeded && retryCount <= MAX_ROLLBACK_RETRIES) {
            if (retryCount > 0) {
                LOG_WARNING("[Runner] Rollback retry {} for {}", retryCount, executed.symbol);
            }

            // Use the original fill price as estimate for the rollback
            std::string rollbackClOrdId = broker_->sendMarketOrder(
                executed.symbol,
                rollbackSide,
                executed.filledQty,
                executed.avgPrice
            );

            auto status = broker_->waitForOrderCompletion(rollbackClOrdId, ROLLBACK_TIMEOUT_MS);

            if (status == OrderStatus::FILLED) {
                auto rollbackState = broker_->getOrderState(rollbackClOrdId);

                // Check for partial fills - warn but consider it a success if mostly filled
                double fillRatio = rollbackState.cumQty / executed.filledQty;
                if (fillRatio < 0.99) {
                    LOG_WARNING("[Runner] Rollback PARTIAL: clOrdId={}, requested={:.8f}, filled={:.8f} ({:.1f}%)",
                                rollbackClOrdId, executed.filledQty, rollbackState.cumQty, fillRatio * 100.0);
                } else {
                    LOG_INFO("[Runner] Rollback FILLED: clOrdId={}, qty={:.8f}, avgPx={:.8f}",
                             rollbackClOrdId, rollbackState.cumQty, rollbackState.avgPx);
                }
                rollbackSucceeded = true;

            } else if (status == OrderStatus::REJECTED) {
                auto rollbackState = broker_->getOrderState(rollbackClOrdId);
                LOG_ERROR("[Runner] Rollback REJECTED: clOrdId={}, reason={}",
                          rollbackClOrdId, rollbackState.rejectReason);

            } else if (status == OrderStatus::UNKNOWN) {
                LOG_ERROR("[Runner] Rollback TIMEOUT: clOrdId={} - status unknown after {}ms",
                          rollbackClOrdId, ROLLBACK_TIMEOUT_MS);

            } else {
                LOG_ERROR("[Runner] Rollback FAILED: clOrdId={}, status={}",
                          rollbackClOrdId, static_cast<int>(status));
            }

            ++retryCount;
        }

        if (!rollbackSucceeded) {
            LOG_CRITICAL("[Runner] ROLLBACK FAILED for {}: {} {} qty={:.8f}",
                         executed.clOrdId, sideStr, executed.symbol, executed.filledQty);
            allRollbacksSucceeded = false;
            // Continue attempting other rollbacks - don't abort early
        }
    }

    LOG_WARNING("[Runner] ========== ROLLBACK {} ==========",
                allRollbacksSucceeded ? "COMPLETE" : "INCOMPLETE");

    return allRollbacksSucceeded;
}

void Runner::executeArbitrage(const Signal& signal) {
    const auto& startingAsset = strategy_->startingAsset();

    LOG_INFO("[Runner] ========== EXECUTING ARBITRAGE ==========");
    LOG_INFO("[Runner] Mode: {}", config_.liveMode ? "LIVE" : "TEST");
    LOG_INFO("[Runner] Path: {}", signal.description);
    LOG_INFO("[Runner] Theoretical PnL: {:.8f}", signal.pnl);
    LOG_INFO("[Runner] {} Balance: {:.8f}", startingAsset, balance_[startingAsset]);

    // Start persistence sequence for this arbitrage
    std::string parentTradeId = tradePersistence_->startArbitrageSequence();
    const size_t totalLegs = signal.orders.size();

    std::vector<LegResult> results;
    results.reserve(signal.orders.size());

    // Track executed orders for potential rollback
    std::vector<ExecutedOrder> executedOrders;
    executedOrders.reserve(signal.orders.size());

    size_t legIndex = 0;

    for (const auto& order : signal.orders) {
        char side = (order.getWay() == Way::BUY) ? FIX::OE::Side_BUY : FIX::OE::Side_SELL;
        std::string symbol = order.getSymbol().to_str();
        double qty = order.getQty();
        double estPrice = order.getPrice();
        double feeRate = strategy_->getFeeForSymbol(symbol) / 100.0;

        LOG_INFO("[Runner] Leg {}: {} {} @ MARKET, estPrice={:.8f}, qty={:.8f}",
                 legIndex + 1, (side == FIX::OE::Side_BUY ? "BUY" : "SELL"), symbol, estPrice, qty);

        std::string clOrdId = broker_->sendMarketOrder(symbol, side, qty, estPrice);

        auto status = broker_->waitForOrderCompletion(clOrdId, 5000);

        if (status == OrderStatus::REJECTED) {
            auto orderState = broker_->getOrderState(clOrdId);
            LOG_CRITICAL("[Runner] Leg {}: Order {} REJECTED: {}",
                        legIndex + 1, clOrdId, orderState.rejectReason);
            handleExecutionFailure(static_cast<int>(legIndex), clOrdId,
                "Order rejected at leg " + std::to_string(legIndex + 1) + ": " + orderState.rejectReason,
                executedOrders);
        }

        if (status == OrderStatus::UNKNOWN) {
            LOG_CRITICAL("[Runner] Leg {}: Order {} TIMEOUT - status unknown",
                        legIndex + 1, clOrdId);
            handleExecutionFailure(static_cast<int>(legIndex), clOrdId,
                "Order timeout at leg " + std::to_string(legIndex + 1) + " - manual intervention required",
                executedOrders);
        }

        if (status != OrderStatus::FILLED) {
            LOG_CRITICAL("[Runner] Leg {}: Order {} unexpected status={}",
                        legIndex + 1, clOrdId, static_cast<int>(status));
            handleExecutionFailure(static_cast<int>(legIndex), clOrdId,
                "Order failed at leg " + std::to_string(legIndex + 1) + " with status " + std::to_string(static_cast<int>(status)),
                executedOrders);
        }

        auto orderState = broker_->getOrderState(clOrdId);
        double realPrice = orderState.avgPx;
        double realQty = orderState.cumQty;

        if (realQty < qty * 0.99) {
            LOG_CRITICAL("[Runner] Leg {}: Order {} PARTIAL FILL: requested={:.8f}, filled={:.8f}",
                        legIndex + 1, clOrdId, qty, realQty);
            // For partial fills, still track what was executed for rollback
            if (realQty > 0) {
                executedOrders.push_back({
                    .clOrdId = clOrdId,
                    .symbol = symbol,
                    .side = side,
                    .filledQty = realQty,
                    .avgPrice = realPrice
                });
            }
            handleExecutionFailure(static_cast<int>(legIndex), clOrdId,
                "Partial fill at leg " + std::to_string(legIndex + 1) + ": requested " +
                std::to_string(qty) + ", filled " + std::to_string(realQty),
                executedOrders);
        }

        double slippage = (estPrice > 0) ? ((realPrice - estPrice) / estPrice * 100.0) : 0.0;

        LOG_INFO("[Runner] Leg {}: FILLED clOrdId={}", legIndex + 1, clOrdId);
        LOG_INFO("[Runner]   Est  Price: {:.8f} | Real Price: {:.8f} | Slippage: {:+.4f}%",
                 estPrice, realPrice, slippage);
        LOG_INFO("[Runner]   Est  Qty:   {:.8f} | Real Qty:   {:.8f}",
                 qty, realQty);

        // Track successful execution for potential rollback
        executedOrders.push_back({
            .clOrdId = clOrdId,
            .symbol = symbol,
            .side = side,
            .filledQty = realQty,
            .avgPrice = realPrice
        });

        // Determine trade type based on leg position
        TradeType tradeType = (legIndex == 0) ? TradeType::ENTRY :
                              (legIndex == totalLegs - 1) ? TradeType::EXIT :
                              TradeType::INTERMEDIATE;

        // Record trade (PnL will be updated for EXIT trade after calculation)
        tradePersistence_->recordTrade(
            clOrdId,
            parentTradeId,
            tradeType,
            symbol,
            (side == FIX::OE::Side_BUY) ? "BUY" : "SELL",
            estPrice,
            qty,
            realPrice,
            realQty,
            TradeStatus::EXECUTED,
            0.0,  // PnL (updated later for EXIT)
            0.0   // PnL% (updated later for EXIT)
        );

        results.push_back({symbol, order.getWay(), estPrice, realPrice, qty, realQty, feeRate});
        ++legIndex;
    }

    // Calculate and report PnL
    {
        double balanceBefore = balance_[startingAsset];

        balance_ = admin_->fetchAccountBalances();
        double balanceAfter = balance_[startingAsset];
        double actualPnl = balanceAfter - balanceBefore;

        double traceAmount = results[0].realQty;
        if (results[0].way == Way::BUY) {
            traceAmount = results[0].realQty * results[0].realPrice;
        }
        double initialStake = traceAmount;

        for (const auto& r : results) {
            if (r.way == Way::BUY) {
                traceAmount = (traceAmount / r.realPrice) * (1.0 - r.feeRate);
            } else {
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

    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        try {
            // Wait for market data updates based on polling mode
            std::bitset<MAX_SYMBOLS> updatedSymbols;

            switch (config_.pollingMode) {
                case PollingMode::Blocking:
                    // Use timed wait for periodic shutdown checks
                    updatedSymbols = orderBook_.waitForUpdatesWithTimeout(std::chrono::milliseconds(100));
                    if (updatedSymbols.none()) {
                        continue;  // Timeout - check shutdown flag and retry
                    }
                    break;
                case PollingMode::BusyPoll:
                    updatedSymbols = orderBook_.waitForUpdatesSpin(INT_MAX);
                    break;
                case PollingMode::Hybrid:
                default:
                    updatedSymbols = orderBook_.waitForUpdatesSpin(config_.busyPollSpinCount);
                    break;
            }

            auto balanceIt = balance_.find(startingAsset);
            if (balanceIt == balance_.end() || balanceIt->second <= 0) [[unlikely]] {
                LOG_CRITICAL("[Runner] No balance for starting asset '{}' - exiting", startingAsset);
                return;
            }
            const double stake = risk * balanceIt->second;

            std::optional<Signal> sig = strategy_->onMarketDataUpdate(
                updatedSymbols, orderBook_, stake, orderSizer_);

            if (sig.has_value()) [[unlikely]] {
                executeArbitrage(*sig);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[Runner] Error in main loop: {}", e.what());
            break;
        }
    }

    LOG_INFO("[Runner] Shutdown requested, exiting main loop");
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
        config.strategyConfig.minProfitRatio = pt.get<double>("TRIANGULAR_ARB_STRATEGY.minProfitRatio", 1.0001);

        // Runner config
        config.liveMode = pt.get<bool>("TRIANGULAR_ARB_STRATEGY.liveMode", false);
        config.fixMdEndpoint = pt.get<std::string>("FIX_CONNECTION.mdEndpoint", "fix-md.testnet.binance.vision");
        config.fixMdPort = pt.get<int>("FIX_CONNECTION.mdPort", 9000);
        config.fixOeEndpoint = pt.get<std::string>("FIX_CONNECTION.oeEndpoint", "fix-oe.testnet.binance.vision");
        config.fixOePort = pt.get<int>("FIX_CONNECTION.oePort", 9000);
        config.restEndpoint = pt.get<std::string>("FIX_CONNECTION.restEndpoint", "testnet.binance.vision");
        config.apiKey = pt.get<std::string>("FIX_CONNECTION.apiKey");
        config.ed25519KeyPath = pt.get<std::string>("FIX_CONNECTION.ed25519KeyPath");

        // Polling mode
        std::string pollingModeStr = pt.get<std::string>("PERFORMANCE.pollingMode", "hybrid");
        if (pollingModeStr == "blocking") {
            config.pollingMode = PollingMode::Blocking;
        } else if (pollingModeStr == "busy_poll") {
            config.pollingMode = PollingMode::BusyPoll;
        } else {
            config.pollingMode = PollingMode::Hybrid;
        }
        config.busyPollSpinCount = pt.get<int>("PERFORMANCE.busyPollSpinCount", 10000);

        // Persistence config
        config.tradeLogDir = pt.get<std::string>("PERSISTENCE.tradeLogDir", "./trades");

        // Per-symbol fees
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
