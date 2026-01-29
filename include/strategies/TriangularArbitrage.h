#pragma once

#include <vector>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <functional>
#include <bitset>

#include "strategies/circular_arbitrage/ArbitragePath.h"
#include "market_connection/OrderBook.h"
#include "fin/Symbol.h"
#include "fin/Signal.h"
#include "fin/OrderSizer.h"

struct TriangularArbitrageConfig {
    std::string startingAsset;
    double defaultFee = 0.1;
    double risk = 1.0;
    double minProfitRatio = 1.0001;  // Minimum ratio (1.0001 = 0.01% profit)
    std::map<std::string, double> symbolFees;
};

/**
 * TriangularArbitrage - High-performance triangular arbitrage strategy.
 *
 * Optimizations:
 * 1. Lock-free OrderBook with seqlock
 * 2. Integer symbol IDs for O(1) lookups
 * 3. Inverted index for O(U) affected path lookup
 * 4. Pre-cached fee multipliers
 * 5. Bitset-based update tracking
 */
class TriangularArbitrage {
public:
    using FeeFunction = std::function<double(const std::string&)>;

    explicit TriangularArbitrage(const TriangularArbitrageConfig& config);
    virtual ~TriangularArbitrage() = default;

    void discoverRoutes(const std::vector<fin::Symbol>& symbols);
    const std::set<std::string>& subscribedSymbols() const { return stratSymbols_; }

    /**
     * Process market data updates (bitset version - preferred).
     */
    std::optional<Signal> onMarketDataUpdate(
        const std::bitset<MAX_SYMBOLS>& updatedSymbols,
        const OrderBook& orderBook,
        double stake,
        const OrderSizer& sizer);

    const std::string& startingAsset() const { return startingAsset_; }
    double risk() const { return risk_; }
    double getFeeForSymbol(const std::string& symbol) const;

    size_t pathCount() const { return pathPool_.size(); }

private:
    std::string startingAsset_;
    double defaultFee_;
    double risk_;
    double minProfitRatio_;
    std::map<std::string, double> symbolFees_;

    // Cached fee function
    FeeFunction feeFunction_;

    // Path pool with inverted index
    ArbitragePathPool pathPool_;

    std::set<std::string> stratSymbols_;

    std::vector<Order> getPossibleOrders(const std::string& coin, const std::vector<fin::Symbol>& relatedSymbols);
    std::vector<ArbitragePath> computeArbitragePaths(
        const std::vector<fin::Symbol>& symbolsList,
        const std::string& startingAsset,
        int arbitrageDepth);
};
