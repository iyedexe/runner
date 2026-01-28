#pragma once

#include <vector>
#include <array>
#include <string>
#include <memory>
#include <optional>
#include <functional>
#include <numeric>

#include "fin/Order.h"
#include "fin/Signal.h"
#include "fin/OrderSizer.h"
#include "strategies/IStrategy.h"  // For MarketDataView, BidAsk


/**
 * ArbitragePath - Represents a single triangular arbitrage path.
 *
 * ============================================================================
 * TRADING RULES (Binance Spot)
 * ============================================================================
 *
 * Symbol Convention:
 *   Symbol BASE/QUOTE (e.g., BTCJPY means BTC=base, JPY=quote)
 *   - To BUY 1 BASE, you pay ASK price in QUOTE
 *   - To SELL 1 BASE, you receive BID price in QUOTE
 *
 * Price Rules:
 *   - BUY  uses ASK price (you pay the higher price)
 *   - SELL uses BID price (you receive the lower price)
 *
 * Fee Rules:
 *   - BUY:  fee deducted from BASE  (what you receive)
 *   - SELL: fee deducted from QUOTE (what you receive)
 *
 * Trade Formulas:
 *   BUY BASE/QUOTE:
 *     input:  stake in QUOTE
 *     output: (stake / ask) * (1 - fee) in BASE
 *
 *   SELL BASE/QUOTE:
 *     input:  stake in BASE
 *     output: (stake * bid) * (1 - fee) in QUOTE
 *
 * ============================================================================
 * EXAMPLE: 1 BTC -> LPT -> JPY -> BTC
 * ============================================================================
 *
 * Leg 1: BUY LPTBTC (LPT/BTC) at ask=0.00003620
 *   input:  1 BTC (quote)
 *   raw:    1 / 0.00003620 = 27624.31 LPT
 *   output: 27624.31 * 0.999 = 27596.68 LPT (after 0.1% fee on base)
 *
 * Leg 2: SELL LPTJPY (LPT/JPY) at bid=513.90
 *   input:  27596.68 LPT (base)
 *   raw:    27596.68 * 513.90 = 14181,423.49 JPY
 *   output: 14181423.49 * 0.999 = 14167242.07 JPY (after 0.1% fee on quote)
 *
 * Leg 3: BUY BTCJPY (BTC/JPY) at ask=13807098
 *   input:  14167242.07 JPY (quote)
 *   raw:    14167242.07 / 13807098 = 1.02608 BTC
 *   output: 1.02608 * 0.999 = 1.02506 BTC (after 0.1% fee on base)
 *
 * PnL: 1.02506 - 1.0 = +0.02506 BTC (+2.506%)
 *
 * ============================================================================
 * PRICE MULTIPLIERS (why 1/ASK for BUY?)
 * ============================================================================
 *
 * For symbol BASE/QUOTE (e.g., BTCJPY: BTC=base, JPY=quote):
 *
 *   ASK = price to buy 1 BASE in QUOTE units
 *   BID = price to sell 1 BASE in QUOTE units
 *
 * BUY scenario:
 *   - To buy 1 BASE, you spend ASK QUOTE    →  1 BASE costs ASK QUOTE
 *   - If you have 1 QUOTE, you get 1/ASK BASE
 *   - Multiplier: 1/ASK (converts QUOTE → BASE)
 *
 * SELL scenario:
 *   - To sell 1 BASE, you receive BID QUOTE  →  1 BASE yields BID QUOTE
 *   - If you have 1 BASE, you get BID QUOTE
 *   - Multiplier: BID (converts BASE → QUOTE)
 *
 * ============================================================================
 * FAST RATIO (O(1) approximation)
 * ============================================================================
 *
 * For quick screening, we compute ratio by applying fee at EACH leg:
 *
 *   ratio = 1.0
 *   for each leg:
 *       ratio *= priceMultiplier[leg] * (1 - fee[leg])
 *
 * Where priceMultiplier is:
 *   - BUY:  1/ask  (converts quote→base)
 *   - SELL: bid    (converts base→quote)
 *
 * If ratio > 1.0, the path is potentially profitable.
 * Note: Fast ratio ignores order size constraints and rounding.
 *
 * ============================================================================
 */
class ArbitragePath {
public:
    using FeeFunction = std::function<double(const std::string&)>;

    ArbitragePath(
        std::vector<Order> orders,
        const FeeFunction& getFee);

    /**
     * Get the symbols involved in this path.
     */
    const std::array<std::string, 3>& symbols() const { return symbols_; }

    /**
     * Get the underlying orders.
     */
    const std::vector<Order>& orders() const { return orders_; }

    /**
     * Update price multipliers from prices map.
     * Call this when any of this path's symbols receive new data.
     */
    void updatePrices(const std::unordered_map<std::string, BidAsk>& prices);

    /**
     * Fast O(1) profitability ratio.
     * Returns finalAmount/initialAmount. Profitable if > 1.0.
     */
    double getFastRatio() const;

    /**
     * Rigorous path evaluation with full order sizing and filter validation.
     * Returns a Signal with theoretical PNL if profitable, nullopt otherwise.
     */
    std::optional<Signal> evaluate(
        double initialStake,
        const std::unordered_map<std::string, BidAsk>& prices,
        const OrderSizer& orderSizer,
        const FeeFunction& getFee) const;

    /**
     * Get path description string.
     */
    std::string description() const;

private:
    std::vector<Order> orders_;
    std::array<std::string, 3> symbols_;
    std::array<bool, 3> isBuy_;  // true if BUY, false if SELL

    // Pre-computed coefficients for fast evaluation
    // multipliers_[i*2] = bid (for SELL), multipliers_[i*2+1] = 1/ask (for BUY)
    std::array<double, 6> multipliers_;
    std::array<size_t, 3> priceIndices_;  // Which multiplier to use per leg
    std::array<double, 3> feeMultipliers_;  // (1 - fee/100) per leg, applied at each transaction

    // Raw prices for logging
    std::array<double, 3> bids_;
    std::array<double, 3> asks_;
};
