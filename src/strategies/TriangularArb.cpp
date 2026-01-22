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

    LOG_INFO("[TriangularArb] Creating REST client for: {}", config.restEndpoint);
    restClient_ = std::make_unique<BNB::REST::ApiClient>(config.restEndpoint, config.apiKey, *key_);

    initialize();
}

void TriangularArb::initialize() {
    LOG_INFO("[TriangularArb] Initialized with starting coin: {}", startingAsset_);

    // Fetch comprehensive symbol and filter data from REST API first
    fetchExchangeInfo();

    LOG_INFO("[TriangularArb] Connecting FIX sessions...");
    feeder_->connect();
    broker_->connect();

    LOG_INFO("[TriangularArb] Waiting for FIX logon...");
    feeder_->waitUntilConnected();
    broker_->waitUntilConnected();

    LOG_INFO("[TriangularArb] FIX sessions connected");

    discoverArbitrageRoutes();
}

void TriangularArb::discoverArbitrageRoutes() {
    LOG_INFO("[TriangularArb] Discovering arbitrage routes...");
    LOG_INFO("[TriangularArb] Using {} symbols from exchange info", symbolsList_.size());

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

    // Fetch account balances from REST API
    fetchAccountBalances();

    // Subscribe to market data
    if (!stratSymbols_.empty()) {
        // Set expected symbols in the store before subscribing
        std::vector<std::string> symbolsVec(stratSymbols_.begin(), stratSymbols_.end());
        feeder_->getMarketDataStore().setExpectedSymbols(symbolsVec);

        LOG_INFO("[TriangularArb] Subscribing to market data for {} symbols", stratSymbols_.size());
        feeder_->subscribeToSymbols(symbolsVec);

        // Wait for all market data snapshots to arrive before starting
        waitForMarketDataSnapshots();
    }
}

void TriangularArb::fetchAccountBalances() {
    LOG_INFO("[TriangularArb] Fetching account balances from REST API...");

    try {
        nlohmann::json response = restClient_->sendRequest(
            BNB::REST::Endpoints::Account::AccountInformation()
                .omitZeroBalances(true)
        );

        // Clear and rebuild balance map
        balance_.clear();

        if (!response.contains("balances")) {
            LOG_WARNING("[TriangularArb] Account response missing 'balances' field");
            return;
        }

        const auto& balances = response["balances"];
        for (const auto& bal : balances) {
            std::string asset = bal.value("asset", "");
            double free = 0.0;

            // Handle both string and number formats
            if (bal.contains("free")) {
                if (bal["free"].is_string()) {
                    free = std::stod(bal["free"].get<std::string>());
                } else {
                    free = bal["free"].get<double>();
                }
            }

            if (!asset.empty() && free > 0) {
                balance_[asset] = free;
                LOG_DEBUG("[TriangularArb] Balance: {} = {}", asset, free);
            }
        }

        LOG_INFO("[TriangularArb] Loaded {} non-zero balances", balance_.size());

        // Verify we have the starting asset
        if (balance_.find(startingAsset_) == balance_.end()) {
            LOG_WARNING("[TriangularArb] No balance found for starting asset: {}", startingAsset_);
            balance_[startingAsset_] = 0.0;
        } else {
            LOG_INFO("[TriangularArb] Starting asset {} balance: {}", startingAsset_, balance_[startingAsset_]);
        }

    } catch (const std::exception& e) {
        LOG_ERROR("[TriangularArb] Failed to fetch account balances: {}", e.what());
        // Don't throw - strategy can still run with test balances
        balance_[startingAsset_] = 0.0;
    }
}

void TriangularArb::waitForMarketDataSnapshots() {
    LOG_INFO("[TriangularArb] Waiting for market data snapshots...");

    auto& store = feeder_->getMarketDataStore();

    // Wait up to 30 seconds for all snapshots
    bool success = store.waitForAllSnapshots(30000);

    auto [received, expected] = store.getSnapshotProgress();
    if (success) {
        LOG_INFO("[TriangularArb] All market data snapshots received ({}/{})", received, expected);
    } else {
        LOG_WARNING("[TriangularArb] Timeout waiting for snapshots, received {}/{}", received, expected);
    }
}

::Symbol TriangularArb::createSymbol(const TriArb::SymbolInfo& info) {
    int precision = 0;
    if (info.stepSize > 0) {
        precision = static_cast<int>(-std::log10(info.stepSize));
    }
    SymbolFilter filter(info.minQty, info.maxQty, info.stepSize, precision);
    return ::Symbol(info.baseAsset, info.quoteAsset, info.symbol, filter);
}

void TriangularArb::fetchExchangeInfo() {
    LOG_INFO("[TriangularArb] Fetching exchange info from REST API...");

    try {
        // Request exchange info for all SPOT symbols
        nlohmann::json response = restClient_->sendRequest(
            BNB::REST::Endpoints::General::ExchangeInfo()
                .permissions({"SPOT"})
        );

        // The response should have a "symbols" array
        if (!response.contains("symbols")) {
            LOG_ERROR("[TriangularArb] Exchange info response missing 'symbols' field");
            return;
        }

        const auto& symbols = response["symbols"];
        LOG_INFO("[TriangularArb] Parsing {} symbols from REST API exchange info", symbols.size());

        // Clear and rebuild symbols list from exchange info (more precise than FIX)
        symbolsList_.clear();
        orderSizer_.clear();

        int parsedCount = 0;
        for (const auto& symbolData : symbols) {
            if (!symbolData.contains("symbol") || !symbolData.contains("filters")) {
                continue;
            }

            // Skip non-TRADING symbols
            if (symbolData.contains("status") && symbolData["status"].get<std::string>() != "TRADING") {
                continue;
            }

            std::string symbolStr = symbolData["symbol"].get<std::string>();
            std::string baseAsset = symbolData.value("baseAsset", "");
            std::string quoteAsset = symbolData.value("quoteAsset", "");

            if (baseAsset.empty() || quoteAsset.empty()) {
                continue;
            }

            // Parse comprehensive filters from the JSON
            SymbolFilters filters = SymbolFilters::fromJson(symbolData["filters"]);
            orderSizer_.addSymbol(symbolStr, filters);

            // Create Symbol object with basic filter for backward compatibility
            SymbolFilter basicFilter(
                filters.lotSize().minQty,
                filters.lotSize().maxQty,
                filters.lotSize().stepSize,
                filters.qtyPrecision()
            );
            symbolsList_.emplace_back(baseAsset, quoteAsset, symbolStr, basicFilter);

            parsedCount++;

            LOG_DEBUG("[TriangularArb] Loaded {}: "
                     "priceFilter(tick={}), lotSize(min={}, max={}, step={})",
                     symbolStr,
                     filters.priceFilter().tickSize,
                     filters.lotSize().minQty,
                     filters.lotSize().maxQty,
                     filters.lotSize().stepSize);
        }

        LOG_INFO("[TriangularArb] Successfully loaded {} symbols from exchange info",
                 parsedCount);

    } catch (const std::exception& e) {
        LOG_ERROR("[TriangularArb] Failed to fetch exchange info: {}", e.what());
        throw;
    }
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
        config.restEndpoint = pt.get<std::string>("FIX_CONNECTION.restEndpoint", "testnet.binance.vision");
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

    // Get current balance for the starting asset
    auto balanceIt = balance_.find(pathStartingAsset);
    if (balanceIt == balance_.end() || balanceIt->second <= 0) {
        LOG_DEBUG("[TriangularArb] No balance for starting asset {}", pathStartingAsset);
        return std::nullopt;
    }

    // Initial stake based on holdings and risk factor
    double initialStake = risk_ * balanceIt->second;
    double currentAmount = initialStake;  // Amount available for next leg

    for (auto& order : path) {
        if (currentAmount <= 0) {
            LOG_DEBUG("[TriangularArb] Current amount is zero/negative, cannot proceed");
            return std::nullopt;
        }

        std::string symbolStr = order.getSymbol().to_str();

        // Query the MarketDataStore for the latest prices
        MarketData marketData = feeder_->getMarketDataStore().get(symbolStr);

        if (marketData.bestBidPrice <= 0 || marketData.bestAskPrice <= 0) {
            LOG_DEBUG("[TriangularArb] Invalid prices for [{}]: bid={}, ask={}",
                     symbolStr, marketData.bestBidPrice, marketData.bestAskPrice);
            return std::nullopt;
        }

        double orderPrice = 0;
        double orderQty = 0;
        double resultingAmount = 0;

        bool useOrderSizer = orderSizer_.hasSymbol(symbolStr);

        if (order.getWay() == Way::SELL) {
            // SELL: we have base asset, want quote asset
            // currentAmount is in base asset units
            orderPrice = marketData.bestBidPrice;

            // Round quantity to valid step size
            if (useOrderSizer) {
                orderQty = orderSizer_.roundQuantity(symbolStr, currentAmount, true);
            } else {
                orderQty = order.getSymbol().getFilter().roundQty(currentAmount);
            }

            // Validate and adjust if needed
            if (useOrderSizer) {
                auto adjusted = orderSizer_.adjustOrder(symbolStr, orderPrice, orderQty, true, orderPrice);

                // If quantity doesn't meet filters, try reducing
                int reductionAttempts = 0;
                while (!adjusted.validation && reductionAttempts < 5) {
                    // Reduce quantity by step size
                    double stepSize = orderSizer_.getFilters(symbolStr).lotSize().stepSize;
                    if (stepSize <= 0) stepSize = orderQty * 0.01;  // 1% reduction fallback
                    orderQty -= stepSize;
                    orderQty = orderSizer_.roundQuantity(symbolStr, orderQty, true);

                    if (orderQty <= 0) break;
                    adjusted = orderSizer_.adjustOrder(symbolStr, orderPrice, orderQty, true, orderPrice);
                    reductionAttempts++;
                }

                if (!adjusted.validation) {
                    LOG_DEBUG("[TriangularArb] SELL order validation failed for {}: {}", symbolStr, adjusted.validation.reason);
                    return std::nullopt;
                }
                orderQty = adjusted.quantity;
            }

            // Result: we get quote asset
            resultingAmount = orderQty * orderPrice;

        } else {  // Way::BUY
            // BUY: we have quote asset, want base asset
            // currentAmount is in quote asset units
            orderPrice = marketData.bestAskPrice;

            // Calculate how much base asset we can buy with our quote asset
            double rawQty = currentAmount / orderPrice;

            // Round quantity to valid step size
            if (useOrderSizer) {
                orderQty = orderSizer_.roundQuantity(symbolStr, rawQty, true);
            } else {
                orderQty = order.getSymbol().getFilter().roundQty(rawQty);
            }

            // Validate and adjust if needed
            if (useOrderSizer) {
                auto adjusted = orderSizer_.adjustOrder(symbolStr, orderPrice, orderQty, true, orderPrice);

                // If quantity doesn't meet filters, try reducing
                int reductionAttempts = 0;
                while (!adjusted.validation && reductionAttempts < 5) {
                    // Reduce quantity by step size
                    double stepSize = orderSizer_.getFilters(symbolStr).lotSize().stepSize;
                    if (stepSize <= 0) stepSize = orderQty * 0.01;  // 1% reduction fallback
                    orderQty -= stepSize;
                    orderQty = orderSizer_.roundQuantity(symbolStr, orderQty, true);

                    if (orderQty <= 0) break;
                    adjusted = orderSizer_.adjustOrder(symbolStr, orderPrice, orderQty, true, orderPrice);
                    reductionAttempts++;
                }

                if (!adjusted.validation) {
                    LOG_DEBUG("[TriangularArb] BUY order validation failed for {}: {}", symbolStr, adjusted.validation.reason);
                    return std::nullopt;
                }
                orderQty = adjusted.quantity;
            }

            // Result: we get base asset
            resultingAmount = orderQty;
        }

        LOG_DEBUG("[TriangularArb] Transaction: {} {} -> {} {} (price={}, qty={})",
                 currentAmount, order.getStartingAsset(),
                 resultingAmount, order.getResultingAsset(),
                 orderPrice, orderQty);

        order.setPrice(orderPrice);
        order.setQty(orderQty);
        order.setType(OrderType::MARKET);

        // Apply fee and propagate to next leg
        double fee = getFeeForSymbol(symbolStr);
        currentAmount = resultingAmount * (1 - fee / 100);
        LOG_DEBUG("[TriangularArb] Amount after fees ({}%): {}", fee, currentAmount);
    }

    // Calculate PnL: final amount minus initial stake
    double pnl = currentAmount - initialStake;
    if (pnl > 0) {
        return Signal(path, pathDescription, pnl);
    }
    return std::nullopt;
}

std::optional<Signal> TriangularArb::onMarketData(const MarketData& data) {
    // Market data is now stored only in MarketDataStore (via Feeder)
    // No local cache - always query the store for latest prices

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
        double price = order.getPrice();

        // Use OrderSizer to validate and potentially adjust the order
        if (orderSizer_.hasSymbol(symbol)) {
            auto adjusted = orderSizer_.adjustOrder(symbol, price, qty, true);  // true = market order

            if (!adjusted.validation) {
                LOG_ERROR("[TriangularArb] Order validation failed for {}: {}, skipping",
                         symbol, adjusted.validation.reason);
                continue;
            }

            if (adjusted.wasAdjusted) {
                LOG_INFO("[TriangularArb] Order adjusted for {}: qty {} -> {}",
                        symbol, qty, adjusted.quantity);
                qty = adjusted.quantity;
            }
        }

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
