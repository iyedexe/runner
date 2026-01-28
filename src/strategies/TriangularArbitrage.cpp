#include "strategies/TriangularArbitrage.h"
#include "logger.hpp"

#include <algorithm>

TriangularArbitrage::TriangularArbitrage(const TriangularArbitrageConfig& config)
    : startingAsset_(config.startingAsset)
    , defaultFee_(config.defaultFee)
    , risk_(config.risk)
    , symbolFees_(config.symbolFees)
{
    LOG_INFO("[TriangularArbitrage] Created with starting asset: {}, defaultFee: {}%, risk: {}",
             startingAsset_, defaultFee_, risk_);
}

double TriangularArbitrage::getFeeForSymbol(const std::string& symbol) const {
    auto it = symbolFees_.find(symbol);
    return (it != symbolFees_.end()) ? it->second : defaultFee_;
}

void TriangularArbitrage::discoverRoutes(const std::vector<fin::Symbol>& symbols) {
    LOG_INFO("[TriangularArbitrage] Discovering arbitrage routes...");
    LOG_INFO("[TriangularArbitrage] Using {} symbols from exchange info", symbols.size());

    auto stratPaths = computeArbitragePaths(symbols, startingAsset_, 3);

    // Create fee function for ArbitragePath
    auto getFee = [this](const std::string& symbol) -> double {
        return getFeeForSymbol(symbol);
    };

    paths_.clear();
    stratSymbols_.clear();

    for (auto& pathOrders : stratPaths) {
        auto path = std::make_shared<ArbitragePath>(pathOrders.orders(), getFee);
        paths_.push_back(path);

        for (const auto& symbol : path->symbols()) {
            stratSymbols_.insert(symbol);
        }
    }

    LOG_INFO("[TriangularArbitrage] Found {} arbitrage paths, {} unique symbols",
             paths_.size(), stratSymbols_.size());
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

    // Create fee function for ArbitragePath construction
    auto getFee = [this](const std::string& symbol) -> double {
        return getFeeForSymbol(symbol);
    };

    std::vector<ArbitragePath> resultPaths;
    resultPaths.reserve(stratPaths.size());

    for (auto& path : stratPaths) {
        resultPaths.emplace_back(std::move(path), getFee);
    }

    LOG_INFO("[TriangularArbitrage] Number of arbitrage paths: {} of depth {}, starting from asset {}",
             resultPaths.size(), arbitrageDepth, startingAsset);
    return resultPaths;
}

std::optional<Signal> TriangularArbitrage::onMarketDataBatch(
    const std::unordered_map<std::string, BidAsk>& prices,
    double stake,
    const OrderSizer& sizer)
{
    if (stake <= 0) {
        return std::nullopt;
    }

    if (paths_.empty()) {
        return std::nullopt;
    }

    // Fee function for evaluation
    auto getFee = [this](const std::string& symbol) -> double {
        return getFeeForSymbol(symbol);
    };

    // Update prices and screen all paths
    std::optional<Signal> bestSignal;
    double bestPnl = 0.0;

    for (auto& path : paths_) {
        // Update path with latest prices
        path->updatePrices(prices);

        // Fast screen: skip paths that aren't profitable
        double ratio = path->getFastRatio();
        if (ratio <= 1.0) {
            continue;
        }

        // Full evaluation with order sizing
        auto signal = path->evaluate(stake, prices, sizer, getFee);
        if (signal.has_value() && signal->pnl > bestPnl) {
            bestPnl = signal->pnl;
            bestSignal = std::move(signal);
        }
    }

    if (bestSignal.has_value()) {
        LOG_INFO("[TriangularArbitrage] Found opportunity: {} with pnl={:.8f}",
                 bestSignal->description, bestSignal->pnl);
    }

    return bestSignal;
}
