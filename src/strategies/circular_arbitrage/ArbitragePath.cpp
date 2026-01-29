#include "strategies/circular_arbitrage/ArbitragePath.h"
#include "logger.hpp"

#include <sstream>

ArbitragePath::ArbitragePath(
    std::vector<Order> orders,
    const FeeFunction& getFee)
    : orders_(std::move(orders))
{
    auto& registry = SymbolRegistry::instance();

    for (size_t leg = 0; leg < 3 && leg < orders_.size(); ++leg) {
        const auto& order = orders_[leg];
        const std::string& symbolStr = order.getSymbol().to_str();

        // Register symbol and store ID
        symbolIds_[leg] = registry.registerSymbol(symbolStr);
        symbolStrings_[leg] = symbolStr;

        // Store leg direction
        isBuy_[leg] = (order.getWay() == Way::BUY);

        // Pre-compute fee multiplier: (1 - fee/100)
        double feePct = getFee(symbolStr);
        feeMultipliers_[leg] = 1.0 - feePct / 100.0;
    }

    buildDescription();
}

void ArbitragePath::buildDescription() {
    std::ostringstream oss;
    for (size_t i = 0; i < orders_.size(); ++i) {
        if (i > 0) oss << " ";
        oss << orders_[i].to_str();
    }
    cachedDescription_ = oss.str();
}

void ArbitragePath::updatePrices(const OrderBook& orderBook) {
    BidAsk p0, p1, p2;

    // Batch read with prefetch optimization
    orderBook.getTriple(symbolIds_[0], symbolIds_[1], symbolIds_[2], p0, p1, p2);

    bids_[0] = p0.bid;
    bids_[1] = p1.bid;
    bids_[2] = p2.bid;
    asks_[0] = p0.ask;
    asks_[1] = p1.ask;
    asks_[2] = p2.ask;

    // Compute effective multipliers for ratio calculation
    pricesValid_ = true;

    for (size_t leg = 0; leg < 3; ++leg) {
        if (isBuy_[leg]) {
            if (asks_[leg] > 0) [[likely]] {
                effectiveMultipliers_[leg] = 1.0 / asks_[leg];
            } else {
                effectiveMultipliers_[leg] = 0.0;
                pricesValid_ = false;
            }
        } else {
            if (bids_[leg] > 0) [[likely]] {
                effectiveMultipliers_[leg] = bids_[leg];
            } else {
                effectiveMultipliers_[leg] = 0.0;
                pricesValid_ = false;
            }
        }
    }
}

double ArbitragePath::getFastRatio() const noexcept {
    if (!pricesValid_) [[unlikely]] {
        return 0.0;
    }

    // Compute ratio: product of (effective_multiplier * fee_multiplier)
    double ratio = 1.0;
    ratio *= effectiveMultipliers_[0] * feeMultipliers_[0];
    ratio *= effectiveMultipliers_[1] * feeMultipliers_[1];
    ratio *= effectiveMultipliers_[2] * feeMultipliers_[2];

    return ratio;
}

std::optional<Signal> ArbitragePath::evaluate(
    double initialStake,
    const OrderBook& orderBook,
    const OrderSizer& orderSizer,
    const FeeFunction& getFee) const
{
    std::vector<Order> workingOrders = orders_;
    double currentAmount = initialStake;

    // Fee rate as decimal (e.g., 0.001 for 0.1%)
    const double feeRate = 1.0 - feeMultipliers_[0];

    for (size_t leg = 0; leg < 3; ++leg) {
        auto& order = workingOrders[leg];
        const std::string& symbol = symbolStrings_[leg];

        // Use cached prices
        double bid = bids_[leg];
        double ask = asks_[leg];

        if (bid <= 0 || ask <= 0) [[unlikely]] {
            return std::nullopt;
        }

        const bool isBuy = isBuy_[leg];
        double orderPrice = isBuy ? ask : bid;

        if (isBuy) {
            // BUY: give quote, get base
            // get = startingQty / ask, then apply fee to what we get
            double rawGetQty = currentAmount / orderPrice;
            double endingQty = rawGetQty * (1.0 - feeRate);

            // Round ending qty to lot size for validation
            double roundedEndingQty = orderSizer.hasSymbol(symbol)
                ? orderSizer.roundQuantity(symbol, endingQty, true)
                : order.getSymbol().getFilters().roundQty(endingQty);

            if (roundedEndingQty <= 0) [[unlikely]] {
                return std::nullopt;
            }

            order.setPrice(orderPrice);
            order.setQty(rawGetQty);
            order.setType(OrderType::MARKET);

            currentAmount = endingQty;
        } else {
            // SELL: give base, get quote
            // Round the qty we're selling to lot size
            double roundedSellQty = orderSizer.hasSymbol(symbol)
                ? orderSizer.roundQuantity(symbol, currentAmount, true)
                : order.getSymbol().getFilters().roundQty(currentAmount);

            if (roundedSellQty <= 0) [[unlikely]] {
                return std::nullopt;
            }

            double rawGetQty = roundedSellQty * orderPrice;
            double endingQty = rawGetQty * (1.0 - feeRate);

            order.setPrice(orderPrice);
            order.setQty(roundedSellQty);
            order.setType(OrderType::MARKET);

            currentAmount = endingQty;
        }
    }

    const double pnl = currentAmount - initialStake;

    if (pnl > 0) [[unlikely]] {
        return Signal(std::move(workingOrders), cachedDescription_, pnl);
    }

    return std::nullopt;
}

// ArbitragePathPool implementation

size_t ArbitragePathPool::addPath(std::shared_ptr<ArbitragePath> path) {
    size_t index = paths_.size();
    paths_.push_back(std::move(path));
    return index;
}

void ArbitragePathPool::buildIndex() {
    for (auto& vec : symbolToPathIndex_) {
        vec.clear();
    }

    for (size_t pathIdx = 0; pathIdx < paths_.size(); ++pathIdx) {
        const auto& path = paths_[pathIdx];
        for (SymbolId symId : path->symbolIds()) {
            symbolToPathIndex_[symId].push_back(pathIdx);
        }
    }

    LOG_INFO("[ArbitragePathPool] Built index for {} paths", paths_.size());
}

std::vector<size_t> ArbitragePathPool::getAffectedPaths(
    const std::bitset<MAX_SYMBOLS>& updatedSymbols) const
{
    std::vector<bool> affected(paths_.size(), false);
    std::vector<size_t> result;
    result.reserve(64);

    for (size_t symId = 0; symId < MAX_SYMBOLS; ++symId) {
        if (updatedSymbols.test(symId)) {
            const auto& pathIndices = symbolToPathIndex_[symId];
            for (size_t pathIdx : pathIndices) {
                if (!affected[pathIdx]) {
                    affected[pathIdx] = true;
                    result.push_back(pathIdx);
                }
            }
        }
    }

    return result;
}
