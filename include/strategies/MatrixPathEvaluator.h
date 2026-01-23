#pragma once

#include <vector>
#include <array>
#include <string>
#include <map>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cmath>
#include "strategies/IStrategy.h"
#include "fin/Order.h"

namespace TriArb {

/**
 * Pre-computed coefficients for a single arbitrage path.
 * Enables fast vectorized PnL computation without re-traversing path structure.
 */
struct PathCoefficients {
    std::array<size_t, 3> symbolIndices;  // Index into price arrays for each leg
    std::array<bool, 3> useBid;           // true=SELL (use bid), false=BUY (use ask)
    double feeMultiplier;                 // Product of (1 - fee_i/100) for all legs
    size_t numLegs;                       // Number of legs (typically 3 for triangular)
};

/**
 * Matrix-based path evaluator for fast approximate PnL computation.
 *
 * Pre-computes path coefficients at initialization time, then uses vectorized
 * price lookups to evaluate all paths in O(n) time where n = number of paths.
 *
 * This class provides:
 * - Fast approximate PnL without filter validation (for screening)
 * - Symbol-to-path mapping for efficient affected-path lookup
 * - Top-K candidate selection for detailed validation
 */
class MatrixPathEvaluator {
public:
    MatrixPathEvaluator() = default;
    ~MatrixPathEvaluator() = default;

    /**
     * Initialize the evaluator with paths and symbols.
     * Pre-computes path coefficients and builds symbol-to-path index.
     *
     * @param paths All arbitrage paths to evaluate
     * @param allSymbols List of all symbols (determines index mapping)
     * @param getFee Function to retrieve fee for a symbol
     */
    void initialize(
        const std::vector<std::vector<Order>>& paths,
        const std::vector<std::string>& allSymbols,
        const std::function<double(const std::string&)>& getFee)
    {
        // Build symbol to index mapping
        symbolToIndex_.clear();
        for (size_t i = 0; i < allSymbols.size(); ++i) {
            symbolToIndex_[allSymbols[i]] = i;
        }

        // Initialize price arrays
        bidPrices_.resize(allSymbols.size(), 0.0);
        askPrices_.resize(allSymbols.size(), 0.0);

        // Pre-compute path coefficients
        pathCoeffs_.clear();
        pathCoeffs_.reserve(paths.size());
        symbolToPaths_.clear();

        for (size_t pathIdx = 0; pathIdx < paths.size(); ++pathIdx) {
            const auto& path = paths[pathIdx];
            PathCoefficients coeffs;
            coeffs.numLegs = path.size();
            coeffs.feeMultiplier = 1.0;

            for (size_t leg = 0; leg < path.size() && leg < 3; ++leg) {
                const auto& order = path[leg];
                std::string symbol = order.getSymbol().to_str();

                auto it = symbolToIndex_.find(symbol);
                if (it == symbolToIndex_.end()) {
                    // Symbol not in index - should not happen with proper setup
                    coeffs.symbolIndices[leg] = 0;
                } else {
                    coeffs.symbolIndices[leg] = it->second;
                }

                coeffs.useBid[leg] = (order.getWay() == Way::SELL);

                double fee = getFee(symbol);
                coeffs.feeMultiplier *= (1.0 - fee / 100.0);

                // Build symbol-to-paths mapping
                symbolToPaths_[symbol].push_back(pathIdx);
            }

            pathCoeffs_.push_back(coeffs);
        }
    }

    /**
     * Update price vectors for specific symbols only.
     * More efficient than updatePrices() when only a few symbols changed.
     *
     * @param symbols List of symbols that were updated
     * @param store Reference to the market data store
     */
    template<typename StoreT>
    void updatePricesSelective(const std::vector<std::string>& symbols, const StoreT& store) {
        for (const auto& symbol : symbols) {
            auto it = symbolToIndex_.find(symbol);
            if (it != symbolToIndex_.end()) {
                size_t idx = it->second;
                MarketData data = store.get(symbol);
                bidPrices_[idx] = data.bestBidPrice;
                askPrices_[idx] = data.bestAskPrice;
            }
        }
    }

    /**
     * Evaluate a single path and return its approximate PnL.
     * Internal helper used by evaluateAffected.
     */
    double evaluatePath(size_t pathIdx, double initialAmount) const {
        const auto& coeffs = pathCoeffs_[pathIdx];
        double amount = initialAmount;

        for (size_t leg = 0; leg < coeffs.numLegs && leg < 3; ++leg) {
            size_t symIdx = coeffs.symbolIndices[leg];
            double price = coeffs.useBid[leg] ? bidPrices_[symIdx] : askPrices_[symIdx];

            if (price <= 0) {
                return -std::numeric_limits<double>::infinity();
            }

            if (coeffs.useBid[leg]) {
                // SELL: amount is in base, result is in quote
                amount = amount * price;
            } else {
                // BUY: amount is in quote, result is in base
                amount = amount / price;
            }
        }

        // Apply cumulative fees
        amount *= coeffs.feeMultiplier;
        return amount - initialAmount;
    }

    /**
     * Evaluate only affected paths and return top-K candidates.
     * More efficient than evaluateAll() when only some paths need re-evaluation.
     *
     * @param affectedPathIndices Set of path indices to evaluate
     * @param initialAmount Starting amount for the arbitrage
     * @param topK Maximum number of candidates to return
     * @param minPnl Minimum PnL threshold (default 0.0)
     * @return Vector of (pnl, pathIdx) pairs sorted by descending PnL
     */
    template<typename SetT>
    std::vector<std::pair<double, size_t>> evaluateAffected(
        const SetT& affectedPathIndices,
        double initialAmount,
        int topK,
        double minPnl = 0.0) const
    {
        std::vector<std::pair<double, size_t>> candidates;
        candidates.reserve(affectedPathIndices.size());

        for (size_t pathIdx : affectedPathIndices) {
            double pnl = evaluatePath(pathIdx, initialAmount);
            if (pnl > minPnl) {
                candidates.emplace_back(pnl, pathIdx);
            }
        }

        // Partial sort to get top K
        if (static_cast<size_t>(topK) < candidates.size()) {
            std::partial_sort(candidates.begin(), candidates.begin() + topK, candidates.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
            candidates.resize(topK);
        } else {
            std::sort(candidates.begin(), candidates.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
        }

        return candidates;
    }

    /**
     * Get path indices affected by a symbol update.
     *
     * @param symbol The symbol that was updated
     * @return Vector of path indices containing this symbol
     */
    const std::vector<size_t>& getPathsForSymbol(const std::string& symbol) const {
        static const std::vector<size_t> empty;
        auto it = symbolToPaths_.find(symbol);
        if (it != symbolToPaths_.end()) {
            return it->second;
        }
        return empty;
    }

    /**
     * Check if evaluator has been initialized.
     */
    bool isInitialized() const {
        return !pathCoeffs_.empty();
    }

    /**
     * Get the number of paths.
     */
    size_t numPaths() const {
        return pathCoeffs_.size();
    }

    /**
     * Get the number of symbols.
     */
    size_t numSymbols() const {
        return symbolToIndex_.size();
    }

private:
    std::vector<PathCoefficients> pathCoeffs_;
    std::vector<double> bidPrices_;
    std::vector<double> askPrices_;
    std::unordered_map<std::string, size_t> symbolToIndex_;
    std::unordered_map<std::string, std::vector<size_t>> symbolToPaths_;
};

} // namespace TriArb
