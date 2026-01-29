#include "strategies/TriangularArbitrage.h"
#include "logger.hpp"

#include <algorithm>

TriangularArbitrage::TriangularArbitrage(const TriangularArbitrageConfig& config)
    : startingAsset_(config.startingAsset)
    , defaultFee_(config.defaultFee)
    , risk_(config.risk)
    , minProfitRatio_(config.minProfitRatio)
    , symbolFees_(config.symbolFees)
{
    // Cache the fee function
    feeFunction_ = [this](const std::string& symbol) -> double {
        return getFeeForSymbol(symbol);
    };

    LOG_INFO("[TriangularArbitrage] Created with starting asset: {}, defaultFee: {}%, risk: {}, minProfitRatio: {}",
             startingAsset_, defaultFee_, risk_, minProfitRatio_);
}

double TriangularArbitrage::getFeeForSymbol(const std::string& symbol) const {
    auto it = symbolFees_.find(symbol);
    return (it != symbolFees_.end()) ? it->second : defaultFee_;
}

void TriangularArbitrage::discoverRoutes(const std::vector<fin::Symbol>& symbols) {
    LOG_INFO("[TriangularArbitrage] Discovering arbitrage routes...");
    LOG_INFO("[TriangularArbitrage] Using {} symbols from exchange info", symbols.size());

    auto stratPaths = computeArbitragePaths(symbols, startingAsset_, 3);

    stratSymbols_.clear();

    for (auto& pathOrders : stratPaths) {
        auto path = std::make_shared<ArbitragePath>(pathOrders.orders(), feeFunction_);
        pathPool_.addPath(path);

        for (const auto& symbol : path->symbols()) {
            stratSymbols_.insert(symbol);
        }
    }

    // Build inverted index for fast affected path lookup
    pathPool_.buildIndex();

    LOG_INFO("[TriangularArbitrage] Found {} arbitrage paths, {} unique symbols",
             pathPool_.size(), stratSymbols_.size());

    // Log all discovered paths with their IDs
    LOG_INFO("[TriangularArbitrage] ========== ARBITRAGE PATHS ==========");
    size_t pathId = 0;
    for (auto& path : pathPool_) {
        const auto& orders = path->orders();
        std::string pathStr;
        for (size_t i = 0; i < orders.size(); ++i) {
            if (i > 0) pathStr += " -> ";
            pathStr += orders[i].getSymbol().to_str();
            pathStr += (orders[i].getWay() == Way::BUY) ? " (BUY)" : " (SELL)";
        }
        LOG_INFO("[TriangularArbitrage] Path {:>4}: {}", pathId, pathStr);
        ++pathId;
    }
    LOG_INFO("[TriangularArbitrage] ======================================");
}

std::vector<Order> TriangularArbitrage::getPossibleOrders(
    const std::string& coin,
    const std::vector<fin::Symbol>& relatedSymbols)
{
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

std::vector<ArbitragePath> TriangularArbitrage::computeArbitragePaths(
    const std::vector<fin::Symbol>& symbolsList,
    const std::string& startingAsset,
    int arbitrageDepth)
{
    LOG_INFO("[TriangularArbitrage] Computing arbitrage paths...");
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

            std::vector<fin::Symbol> unusedSymbols;
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

    std::vector<ArbitragePath> resultPaths;
    resultPaths.reserve(stratPaths.size());

    for (auto& path : stratPaths) {
        resultPaths.emplace_back(std::move(path), feeFunction_);
    }

    LOG_INFO("[TriangularArbitrage] Created {} arbitrage paths of depth {} from asset {}",
             resultPaths.size(), arbitrageDepth, startingAsset);
    return resultPaths;
}

std::optional<Signal> TriangularArbitrage::onMarketDataUpdate(
    const std::bitset<MAX_SYMBOLS>& updatedSymbols,
    const OrderBook& orderBook,
    double stake,
    const OrderSizer& sizer)
{
    if (stake <= 0) [[unlikely]] {
        return std::nullopt;
    }

    if (pathPool_.size() == 0) [[unlikely]] {
        return std::nullopt;
    }

    // Get affected paths using inverted index - O(U) where U = updated symbols
    auto affectedPathIndices = pathPool_.getAffectedPaths(updatedSymbols);

    if (affectedPathIndices.empty()) [[likely]] {
        return std::nullopt;
    }

    std::optional<Signal> bestSignal;
    double bestPnl = 0.0;

    // Fee rate as decimal (e.g., 0.001 for 0.1%)
    const double feeRate = defaultFee_ / 100.0;

    for (size_t pathIdx : affectedPathIndices) {
        auto& path = pathPool_.getPath(pathIdx);

        // Update prices from lock-free book
        path->updatePrices(orderBook);

        // Fast screen
        double ratio = path->getFastRatio();

        if (ratio <= minProfitRatio_) [[likely]] {
            continue;
        }

        // Debug: Log detailed fast ratio computation like user's notes
        const auto& syms = path->symbols();
        const auto& bids = path->cachedBids();
        const auto& asks = path->cachedAsks();
        const auto& dirs = path->legDirections();
        const auto& orders = path->orders();

        LOG_DEBUG("[Eval] Path {:>4} FEE_RATE = {}", pathIdx, feeRate);
        LOG_DEBUG("[Eval] Path {:>4} MD : {} [b={:.8f} a={:.8f}], {} [b={:.8f} a={:.8f}], {} [b={:.8f} a={:.8f}]",
                 pathIdx,
                 syms[0], bids[0], asks[0],
                 syms[1], bids[1], asks[1],
                 syms[2], bids[2], asks[2]);

        // Compute theoretical path step by step (no rounding, just for logging)
        double currentAmount = 1.0;  // Start with 1 unit
        for (size_t leg = 0; leg < 3; ++leg) {
            const auto& order = orders[leg];
            std::string giveAsset = order.getStartingAsset();
            std::string getAsset = order.getResultingAsset();
            double startQty = currentAmount;

            if (dirs[leg]) {
                // BUY: give quote, get base = startQty / ask, fee on get
                double rawGet = startQty / asks[leg];
                double fee = rawGet * feeRate;
                double endQty = rawGet - fee;

                if (leg == 0) {
                    LOG_DEBUG("[Eval] Path {:>4} {}@{} give {{startingQty_{}[{}]=balance[{}]={}}} {}, "
                             "get {{startingQty_{}[{}] / ask[{}] = {} / {} = {}}} {}, "
                             "pay fee {{{} * {} = {}}}, endingQty_{}[{}]={}",
                             pathIdx, "BUY", syms[leg],
                             leg + 1, giveAsset, giveAsset, startQty, giveAsset,
                             leg + 1, giveAsset, syms[leg], startQty, asks[leg], rawGet, getAsset,
                             rawGet, feeRate, fee,
                             leg + 1, getAsset, endQty);
                } else {
                    LOG_DEBUG("[Eval] Path {:>4} {}@{} give {{startingQty_{}[{}]=endingQty_{}[{}]={}}} {}, "
                             "get {{startingQty_{}[{}] / ask[{}] = {} / {} = {}}} {}, "
                             "pay fee {{{} * {} = {}}}, endingQty_{}[{}]={}",
                             pathIdx, "BUY", syms[leg],
                             leg + 1, giveAsset, leg, giveAsset, startQty, giveAsset,
                             leg + 1, giveAsset, syms[leg], startQty, asks[leg], rawGet, getAsset,
                             rawGet, feeRate, fee,
                             leg + 1, getAsset, endQty);
                }
                currentAmount = endQty;
            } else {
                // SELL: give base, get quote = startQty * bid, fee on get
                double rawGet = startQty * bids[leg];
                double fee = rawGet * feeRate;
                double endQty = rawGet - fee;

                if (leg == 0) {
                    LOG_DEBUG("[Eval] Path {:>4} {}@{} give {{startingQty_{}[{}]=balance[{}]={}}} {}, "
                             "get {{startingQty_{}[{}] * bid[{}] = {} * {} = {}}} {}, "
                             "pay fee {{{} * {} = {}}}, endingQty_{}[{}]={}",
                             pathIdx, "SELL", syms[leg],
                             leg + 1, giveAsset, giveAsset, startQty, giveAsset,
                             leg + 1, giveAsset, syms[leg], startQty, bids[leg], rawGet, getAsset,
                             rawGet, feeRate, fee,
                             leg + 1, getAsset, endQty);
                } else {
                    LOG_DEBUG("[Eval] Path {:>4} {}@{} give {{startingQty_{}[{}]=endingQty_{}[{}]={}}} {}, "
                             "get {{startingQty_{}[{}] * bid[{}] = {} * {} = {}}} {}, "
                             "pay fee {{{} * {} = {}}}, endingQty_{}[{}]={}",
                             pathIdx, "SELL", syms[leg],
                             leg + 1, giveAsset, leg, giveAsset, startQty, giveAsset,
                             leg + 1, giveAsset, syms[leg], startQty, bids[leg], rawGet, getAsset,
                             rawGet, feeRate, fee,
                             leg + 1, getAsset, endQty);
                }
                currentAmount = endQty;
            }
        }

        double theoreticalPnl = currentAmount - 1.0;
        double theoreticalPnlPct = theoreticalPnl * 100.0;
        LOG_DEBUG("[Eval] Path {:>4} PNL = {} - 1 = {}/1 = {}%",
                 pathIdx, currentAmount, theoreticalPnl, theoreticalPnlPct);

        // Full evaluation with actual stake and rounding
        auto signal = path->evaluate(stake, orderBook, sizer, feeFunction_);

        if (signal.has_value() && signal->pnl > bestPnl) [[unlikely]] {
            bestPnl = signal->pnl;
            bestSignal = std::move(signal);
        }
    }

    if (bestSignal.has_value()) [[unlikely]] {
        LOG_CRITICAL("[TriangularArbitrage] Found opportunity: {} with pnl={:.8f}",
                 bestSignal->description, bestSignal->pnl);
    }

    return bestSignal;
}
