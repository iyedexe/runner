#include "strategies/TriangularArb.h"
#include "codegen/fix/OE/FixValues.h"
#include "logger.hpp"
#include "crypto/utils.hpp"

TriangularArb::TriangularArb(const TriangularArbConfig& config)
    : config_(config)
    , startingAsset_(config.startingAsset)
    , defaultFee_(config.defaultFee)
    , symbolFees_(config.symbolFees)
    , risk_(config.risk)
{
    LOG_INFO("[TriangularArb] Loading ED25519 key from: {}", config.ed25519KeyPath);
    key_ = std::make_unique<crypto::ed25519>(readPemFile(config.ed25519KeyPath));

    LOG_INFO("[TriangularArb] Creating FIX Feeder");
    feeder_ = std::make_unique<TriArb::Feeder>(config.apiKey, *key_);

    LOG_INFO("[TriangularArb] Creating FIX Broker (liveMode={})", config.liveMode);
    broker_ = std::make_unique<TriArb::Broker>(config.apiKey, *key_, config.liveMode);

    initialize();
}

void TriangularArb::initialize() {
    LOG_INFO("[TriangularArb] Initialized with starting coin: {}", startingAsset_);

    LOG_INFO("[TriangularArb] Connecting FIX sessions...");
    feeder_->connect();
    broker_->connect();

    LOG_INFO("[TriangularArb] Waiting for FIX logon...");
    feeder_->waitUntilConnected();
    broker_->waitUntilConnected();

    LOG_INFO("[TriangularArb] FIX sessions connected, requesting instrument list");
    feeder_->requestInstrumentList();
    feeder_->waitForInstrumentList();

    discoverArbitrageRoutes();
}

void TriangularArb::discoverArbitrageRoutes() {
    LOG_INFO("[TriangularArb] Discovering arbitrage routes...");

    auto symbolInfos = feeder_->getSymbols();
    LOG_INFO("[TriangularArb] Received {} symbols from exchange", symbolInfos.size());

    // Convert SymbolInfo to Symbol objects
    symbolsList_.clear();
    for (const auto& info : symbolInfos) {
        if (!info.baseAsset.empty() && !info.quoteAsset.empty()) {
            symbolsList_.push_back(createSymbol(info));
        }
    }

    LOG_INFO("[TriangularArb] Converted {} valid symbols", symbolsList_.size());

    // Compute arbitrage paths
    stratPaths_ = computeArbitragePaths(symbolsList_, startingAsset_, 3);

    // Collect all symbols needed for market data subscription
    stratSymbols_.clear();
    for (const auto& path : stratPaths_) {
        std::string pathDescription;
        for (const auto& order : path) {
            stratSymbols_.insert(order.getSymbol().to_str());
            pathDescription += order.to_str() + " ";
        }
        LOG_DEBUG("[TriangularArb] Arbitrage path: {}", pathDescription);
    }

    LOG_INFO("[TriangularArb] Found {} arbitrage paths using {} symbols",
             stratPaths_.size(), stratSymbols_.size());

    // Subscribe to market data
    if (!stratSymbols_.empty()) {
        LOG_INFO("[TriangularArb] Subscribing to market data for {} symbols", stratSymbols_.size());
        feeder_->subscribeToSymbols({stratSymbols_.begin(), stratSymbols_.end()});
    }

    // Initialize balances (placeholder - in real implementation would query account)
    balance_[startingAsset_] = 100.0;  // Default starting balance for testing
}

::Symbol TriangularArb::createSymbol(const TriArb::SymbolInfo& info) {
    int precision = 0;
    if (info.stepSize > 0) {
        precision = static_cast<int>(-std::log10(info.stepSize));
    }
    SymbolFilter filter(info.minQty, info.maxQty, info.stepSize, precision);
    return ::Symbol(info.baseAsset, info.quoteAsset, info.symbol, filter);
}

void TriangularArb::shutdown() {
    LOG_INFO("[TriangularArb] Shutting down...");

    if (feeder_) {
        feeder_->disconnect();
    }
    if (broker_) {
        broker_->disconnect();
    }
}

double TriangularArb::getFeeForSymbol(const std::string& symbol) const {
    auto it = symbolFees_.find(symbol);
    if (it != symbolFees_.end()) {
        return it->second;
    }
    return defaultFee_;
}

TriangularArbConfig TriangularArb::loadConfig(const std::string& configFile) {
    TriangularArbConfig config;
    boost::property_tree::ptree pt;

    try {
        boost::property_tree::ini_parser::read_ini(configFile, pt);

        config.startingAsset = pt.get<std::string>("TRIANGULAR_ARB_STRATEGY.startingAsset");
        config.defaultFee = pt.get<double>("TRIANGULAR_ARB_STRATEGY.defaultFee", 0.1);
        config.risk = pt.get<double>("TRIANGULAR_ARB_STRATEGY.risk", 1.0);
        config.liveMode = pt.get<bool>("TRIANGULAR_ARB_STRATEGY.liveMode", false);

        config.fixMdEndpoint = pt.get<std::string>("FIX_CONNECTION.mdEndpoint", "fix-md.testnet.binance.vision");
        config.fixMdPort = pt.get<int>("FIX_CONNECTION.mdPort", 9000);
        config.fixOeEndpoint = pt.get<std::string>("FIX_CONNECTION.oeEndpoint", "fix-oe.testnet.binance.vision");
        config.fixOePort = pt.get<int>("FIX_CONNECTION.oePort", 9000);
        config.apiKey = pt.get<std::string>("FIX_CONNECTION.apiKey");
        config.ed25519KeyPath = pt.get<std::string>("FIX_CONNECTION.ed25519KeyPath");

        // Parse per-symbol fees from [SYMBOL_FEES] section
        auto symbolFeesSection = pt.get_child_optional("SYMBOL_FEES");
        if (symbolFeesSection) {
            for (const auto& item : *symbolFeesSection) {
                config.symbolFees[item.first] = item.second.get_value<double>();
            }
        }

    } catch (const boost::property_tree::ini_parser_error& e) {
        throw std::runtime_error("Failed to load config file: " + std::string(e.what()));
    } catch (const boost::property_tree::ptree_bad_path& e) {
        throw std::runtime_error("Missing parameter in config file: " + std::string(e.what()));
    }

    return config;
}

std::vector<Order> TriangularArb::getPossibleOrders(const std::string& coin, const std::vector<::Symbol>& relatedSymbols) {
    std::vector<Order> orders;
    for (const auto& symbol : relatedSymbols) {
        if (coin == symbol.getBase()) {
            orders.emplace_back(symbol, Way::SELL);
        } else if (coin == symbol.getQuote()) {
            orders.emplace_back(symbol, Way::BUY);
        }
    }
    return orders;
}

std::vector<std::vector<Order>> TriangularArb::computeArbitragePaths(
    const std::vector<::Symbol>& symbolsList,
    const std::string& startingAsset,
    int arbitrageDepth)
{
    LOG_INFO("[TriangularArb] Computing arbitrage paths...");
    std::vector<std::vector<Order>> stratPaths;
    auto firstOrders = getPossibleOrders(startingAsset, symbolsList);

    for (const auto& order : firstOrders) {
        stratPaths.push_back({order});
    }

    for (int i = 0; i < arbitrageDepth - 1; ++i) {
        std::vector<std::vector<Order>> paths;
        for (const auto& path : stratPaths) {
            Order lastOrder = path.back();
            std::string resultingCoin = (lastOrder.getWay() == Way::SELL)
                ? lastOrder.getSymbol().getQuote()
                : lastOrder.getSymbol().getBase();

            std::vector<::Symbol> unusedSymbols;
            for (const auto& symbol : symbolsList) {
                bool used = std::any_of(path.begin(), path.end(),
                    [&symbol](const Order& order) { return order.getSymbol() == symbol; });
                if (!used) {
                    unusedSymbols.push_back(symbol);
                }
            }

            std::vector<Order> possibleNextOrders = getPossibleOrders(resultingCoin, unusedSymbols);
            for (const auto& nextOrder : possibleNextOrders) {
                std::string nextResultingCoin = (nextOrder.getWay() == Way::SELL)
                    ? nextOrder.getSymbol().getQuote()
                    : nextOrder.getSymbol().getBase();

                // On the last step, must return to starting asset
                if ((i == arbitrageDepth - 2) && (nextResultingCoin != startingAsset)) {
                    continue;
                }

                auto newPath = path;
                newPath.push_back(nextOrder);
                paths.push_back(newPath);
            }
        }
        stratPaths = paths;
    }

    LOG_INFO("[TriangularArb] Number of arbitrage paths: {} of depth {}, starting from asset {}",
             stratPaths.size(), arbitrageDepth, startingAsset);
    return stratPaths;
}

std::optional<Signal> TriangularArb::evaluatePath(std::vector<Order>& path) {
    Order firstOrder = path[0];
    std::string pathStartingAsset = firstOrder.getStartingAsset();
    std::string pathDescription = std::accumulate(
        path.begin(),
        path.end(),
        std::string(),
        [](const std::string& acc, const Order& ord) {
            return acc.empty() ? ord.to_str() : acc + " " + ord.to_str();
        });

    LOG_DEBUG("[TriangularArb] Evaluating path: {}", pathDescription);

    double startingAssetQty = 0;
    double resultingAssetQty = risk_ * balance_[pathStartingAsset];

    for (auto& order : path) {
        startingAssetQty = resultingAssetQty;

        if (startingAssetQty == 0) {
            LOG_DEBUG("[TriangularArb] Starting asset qty for {} is null, cannot proceed",
                     order.getStartingAsset());
            return std::nullopt;
        }

        auto it = marketData_.find(order.getSymbol().to_str());
        if (it == marketData_.end()) {
            LOG_DEBUG("[TriangularArb] Market data unavailable for [{}]", order.getSymbol().to_str());
            return std::nullopt;
        }

        const MarketData& marketData = it->second;

        if (marketData.bestBidPrice <= 0 || marketData.bestAskPrice <= 0) {
            LOG_DEBUG("[TriangularArb] Invalid prices for [{}]", order.getSymbol().to_str());
            return std::nullopt;
        }

        double orderPrice = 0;
        double orderQty = 0;

        if (order.getWay() == Way::SELL) {
            orderQty = order.getSymbol().getFilter().roundQty(startingAssetQty);
            resultingAssetQty = orderQty * marketData.bestBidPrice;
            orderPrice = marketData.bestBidPrice;
        }
        if (order.getWay() == Way::BUY) {
            orderQty = order.getSymbol().getFilter().roundQty(startingAssetQty / marketData.bestAskPrice);
            resultingAssetQty = orderQty;
            orderPrice = marketData.bestAskPrice;
        }

        LOG_DEBUG("[TriangularArb] Transaction: {} {} -> {} {}",
                 startingAssetQty, order.getStartingAsset(),
                 resultingAssetQty, order.getResultingAsset());

        order.setPrice(orderPrice);
        order.setQty(orderQty);
        order.setType(OrderType::MARKET);

        double fee = getFeeForSymbol(order.getSymbol().to_str());
        resultingAssetQty *= (1 - fee / 100);
        LOG_DEBUG("[TriangularArb] Amount after fees ({}%): {}", fee, resultingAssetQty);
    }

    double pnl = resultingAssetQty - risk_ * balance_[pathStartingAsset];
    if (pnl > 0) {
        return Signal(path, pathDescription, pnl);
    }
    return std::nullopt;
}

std::optional<Signal> TriangularArb::onMarketData(const MarketData& data) {
    marketData_[data.symbol] = data;

    double maxPnl = 0;
    std::optional<Signal> outSignal;

    for (auto& path : stratPaths_) {
        bool pathAffected = std::any_of(path.begin(), path.end(),
            [&data](const Order& order) {
                return order.getSymbol().to_str() == data.symbol;
            });

        if (pathAffected) {
            std::optional<Signal> sig = evaluatePath(path);
            if (sig.has_value() && sig->pnl > maxPnl) {
                outSignal = sig;
                maxPnl = sig->pnl;
            }
        }
    }

    return outSignal;
}

void TriangularArb::executeArbitrage(const Signal& signal) {
    LOG_INFO("[TriangularArb] Executing arbitrage: {}", signal.description);

    for (const auto& order : signal.orders) {
        char side = (order.getWay() == Way::BUY) ? FIX::OE::Side_BUY : FIX::OE::Side_SELL;
        std::string symbol = order.getSymbol().to_str();
        double qty = order.getQty();

        LOG_INFO("[TriangularArb] Submitting order: {} {} @ MARKET, qty={}",
                 (side == FIX::OE::Side_BUY ? "BUY" : "SELL"), symbol, qty);

        std::string clOrdId;
        if (config_.liveMode) {
            clOrdId = broker_->sendMarketOrder(symbol, side, qty);
        } else {
            clOrdId = broker_->testMarketOrder(symbol, side, qty);
        }

        // Wait for order completion
        auto status = broker_->waitForOrderCompletion(clOrdId, 5000);

        if (status != TriArb::OrderStatus::FILLED) {
            LOG_ERROR("[TriangularArb] Order {} not filled, status={}, aborting arbitrage",
                     clOrdId, static_cast<int>(status));
            break;
        }

        auto orderState = broker_->getOrderState(clOrdId);
        LOG_INFO("[TriangularArb] Order {} filled: cumQty={}", clOrdId, orderState.cumQty);
    }
}

void TriangularArb::run() {
    LOG_INFO("[TriangularArb] Starting main loop...");

    while (true) {
        try {
            MarketData update = feeder_->getUpdate();

            std::optional<Signal> sig = onMarketData(update);

            if (sig.has_value()) {
                LOG_INFO("[TriangularArb] Detected trading signal, theo PNL: {}, description: {}",
                        sig->pnl, sig->description);

                executeArbitrage(*sig);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[TriangularArb] Error in main loop: {}", e.what());
            break;
        }
    }
}
