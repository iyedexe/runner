#pragma once

#include <vector>
#include <array>
#include <string>
#include <memory>
#include <optional>
#include <functional>

#include "market_connection/OrderBook.h"
#include "fin/Order.h"
#include "fin/Signal.h"
#include "fin/OrderSizer.h"

using FeeFunction = std::function<double(const std::string&)>;

/**
 * ArbitragePath - High-performance triangular arbitrage path.
 *
 * Optimizations:
 * 1. Uses SymbolId (integer) for O(1) lookups
 * 2. Pre-computed fee multipliers
 * 3. Cache-aligned data layout
 * 4. Cached description string
 * 5. Batch price reads with prefetch
 */
class ArbitragePath {
public:
    ArbitragePath(std::vector<Order> orders, const FeeFunction& getFee);

    /**
     * Update cached prices from order book (~15ns).
     */
    void updatePrices(const OrderBook& orderBook);

    /**
     * Fast profitability check (~5ns).
     * Returns ratio > 1.0 if potentially profitable.
     */
    [[nodiscard]] double getFastRatio() const noexcept;

    /**
     * Full evaluation with order sizing (~500ns).
     */
    [[nodiscard]] std::optional<Signal> evaluate(
        double initialStake,
        const OrderBook& orderBook,
        const OrderSizer& orderSizer,
        const FeeFunction& getFee) const;

    [[nodiscard]] const std::string& description() const noexcept {
        return cachedDescription_;
    }

    [[nodiscard]] const std::array<std::string, 3>& symbols() const { return symbolStrings_; }
    [[nodiscard]] const std::array<SymbolId, 3>& symbolIds() const { return symbolIds_; }
    [[nodiscard]] const std::vector<Order>& orders() const { return orders_; }

    // Accessors for cached market data (for debug logging)
    [[nodiscard]] const std::array<double, 3>& cachedBids() const noexcept { return bids_; }
    [[nodiscard]] const std::array<double, 3>& cachedAsks() const noexcept { return asks_; }
    [[nodiscard]] const std::array<bool, 3>& legDirections() const noexcept { return isBuy_; }
    [[nodiscard]] const std::array<double, 3>& feeMultipliers() const noexcept { return feeMultipliers_; }

    /**
     * Check if path contains a specific symbol.
     */
    [[nodiscard]] bool containsSymbol(SymbolId id) const noexcept {
        return symbolIds_[0] == id || symbolIds_[1] == id || symbolIds_[2] == id;
    }

private:
    std::vector<Order> orders_;

    // Symbol identifiers
    std::array<SymbolId, 3> symbolIds_;
    std::array<std::string, 3> symbolStrings_;

    // Leg configuration
    std::array<bool, 3> isBuy_;

    // Pre-computed fee multipliers: (1 - fee/100)
    std::array<double, 3> feeMultipliers_;

    // Cached prices
    alignas(32) std::array<double, 3> bids_{0.0, 0.0, 0.0};
    alignas(32) std::array<double, 3> asks_{0.0, 0.0, 0.0};

    // Effective multipliers for ratio computation
    alignas(32) std::array<double, 3> effectiveMultipliers_{0.0, 0.0, 0.0};

    // Cached description
    std::string cachedDescription_;

    // Validity flag
    bool pricesValid_ = false;

    // Working buffers for allocation-free evaluate()
    mutable std::array<double, 3> workingPrices_;
    mutable std::array<double, 3> workingQtys_;

    void buildDescription();
};

/**
 * ArbitragePathPool - Collection with inverted index for O(1) lookup.
 */
class ArbitragePathPool {
public:
    size_t addPath(std::shared_ptr<ArbitragePath> path);
    void buildIndex();

    [[nodiscard]] std::vector<size_t> getAffectedPaths(
        const std::bitset<MAX_SYMBOLS>& updatedSymbols) const;

    [[nodiscard]] std::shared_ptr<ArbitragePath>& getPath(size_t index) {
        return paths_[index];
    }

    [[nodiscard]] size_t size() const noexcept { return paths_.size(); }

    auto begin() { return paths_.begin(); }
    auto end() { return paths_.end(); }

private:
    std::vector<std::shared_ptr<ArbitragePath>> paths_;
    std::array<std::vector<size_t>, MAX_SYMBOLS> symbolToPathIndex_;
};
