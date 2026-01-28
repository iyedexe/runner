#include "strategies/ArbitragePath.h"
#include "logger.hpp"

ArbitragePath::ArbitragePath(
    std::vector<Order> orders,
    const FeeFunction& getFee)
    : orders_(std::move(orders))
{
    multipliers_.fill(0.0);
    feeMultipliers_.fill(1.0);
    bids_.fill(0.0);
    asks_.fill(0.0);
    isBuy_.fill(false);

    for (size_t leg = 0; leg < orders_.size() && leg < 3; ++leg) {
        const auto& order = orders_[leg];
        symbols_[leg] = order.getSymbol().to_str();

        isBuy_[leg] = (order.getWay() == Way::BUY);
        // For SELL: use bid (index leg*2), for BUY: use 1/ask (index leg*2+1)
        priceIndices_[leg] = leg * 2 + (isBuy_[leg] ? 1 : 0);

        // Store per-leg fee multiplier: (1 - fee%) applied at each transaction
        const double feePct = getFee(symbols_[leg]);
        feeMultipliers_[leg] = 1.0 - feePct / 100.0;
    }
}

void ArbitragePath::updatePrices(const std::unordered_map<std::string, BidAsk>& prices) {
    // Store price multipliers for fast ratio calculation:
    //   SELL: bid   (1 BASE -> BID QUOTE)
    //   BUY:  1/ask (1 QUOTE -> 1/ASK BASE)
    for (size_t leg = 0; leg < 3; ++leg) {
        auto it = prices.find(symbols_[leg]);
        if (it != prices.end()) {
            bids_[leg] = it->second.bid;
            asks_[leg] = it->second.ask;
            multipliers_[leg * 2] = it->second.bid;                                      // SELL: BASE->QUOTE
            multipliers_[leg * 2 + 1] = (it->second.ask > 0) ? 1.0 / it->second.ask : 0.0;    // BUY: QUOTE->BASE
        }
    }
}

double ArbitragePath::getFastRatio() const {
    // Fast Ratio: fee applied at EACH transaction
    // BUY: 1/ask, SELL: bid, each multiplied by (1-fee)

    // Check for invalid prices first
    for (size_t leg = 0; leg < 3; ++leg) {
        if (multipliers_[priceIndices_[leg]] <= 0) return 0.0;
    }

    // Compute ratio and track per-leg values for logging
    double running = 1.0;
    std::array<double, 3> outputs;
    for (size_t leg = 0; leg < 3; ++leg) {
        const double mult = multipliers_[priceIndices_[leg]];
        running *= mult * feeMultipliers_[leg];
        outputs[leg] = running;
    }

    // Line 1: Market data
    LOG_DEBUG("[FastPath] MD: {}:b={:.8f}/a={:.8f} | {}:b={:.8f}/a={:.8f} | {}:b={:.8f}/a={:.8f}",
              symbols_[0], bids_[0], asks_[0],
              symbols_[1], bids_[1], asks_[1],
              symbols_[2], bids_[2], asks_[2]);

    // Line 2: Formula - BUY: in/ask*fee=out, SELL: in*bid*fee=out
    // For fast ratio, input is 1.0, then cascades
    double in0 = 1.0, in1 = outputs[0], in2 = outputs[1];
    double p0 = isBuy_[0] ? asks_[0] : bids_[0];
    double p1 = isBuy_[1] ? asks_[1] : bids_[1];
    double p2 = isBuy_[2] ? asks_[2] : bids_[2];

    LOG_DEBUG("[FastPath] {} | {:.6f}{}{:.8f}*{:.4f}={:.6f} | {:.6f}{}{:.8f}*{:.4f}={:.6f} | {:.6f}{}{:.8f}*{:.4f}={:.6f} | ratio={:.6f}",
              description(),
              in0, (isBuy_[0] ? "/" : "*"), p0, feeMultipliers_[0], outputs[0],
              in1, (isBuy_[1] ? "/" : "*"), p1, feeMultipliers_[1], outputs[1],
              in2, (isBuy_[2] ? "/" : "*"), p2, feeMultipliers_[2], outputs[2],
              running);

    return running;
}

std::string ArbitragePath::description() const {
    return std::accumulate(
        orders_.begin(), orders_.end(), std::string(),
        [](const std::string& acc, const Order& ord) {
            return acc.empty() ? ord.to_str() : acc + " " + ord.to_str();
        });
}

std::optional<Signal> ArbitragePath::evaluate(
    double initialStake,
    const std::unordered_map<std::string, BidAsk>& prices,
    const OrderSizer& orderSizer,
    const FeeFunction& getFee) const
{
    std::vector<Order> workingOrders = orders_;
    double currentAmount = initialStake;
    std::string failReason;

    for (size_t leg = 0; leg < workingOrders.size(); ++leg) {
        auto& order = workingOrders[leg];
        const std::string& symbol = order.getSymbol().to_str();
        auto priceIt = prices.find(symbol);
        if (priceIt == prices.end()) {
            failReason = "leg" + std::to_string(leg) + ":" + symbol + " missing price";
            break;
        }
        const auto& price = priceIt->second;
        const bool isBuy = (order.getWay() == Way::BUY);

        if (price.bid <= 0 || price.ask <= 0) {
            failReason = "leg" + std::to_string(leg) + ":" + symbol + " invalid price";
            break;
        }

        const double feeMult = 1.0 - getFee(symbol) / 100.0;
        double orderPrice = isBuy ? price.ask : price.bid;
        double orderQty = 0;

        if (isBuy) {
            // BUY: QUOTE -> BASE
            double rawQty = currentAmount / orderPrice;
            bool hasSizer = orderSizer.hasSymbol(symbol);
            orderQty = hasSizer
                ? orderSizer.roundQuantity(symbol, rawQty, true)
                : order.getSymbol().getFilters().roundQty(rawQty);
            LOG_DEBUG("[Evaluate] {} BUY: raw={:.10f} -> rounded={:.10f} (sizer={})",
                     symbol, rawQty, orderQty, hasSizer);
            currentAmount = orderQty * feeMult;
        } else {
            // SELL: BASE -> QUOTE
            double rawQty = currentAmount;
            bool hasSizer = orderSizer.hasSymbol(symbol);
            orderQty = hasSizer
                ? orderSizer.roundQuantity(symbol, rawQty, true)
                : order.getSymbol().getFilters().roundQty(rawQty);
            LOG_DEBUG("[Evaluate] {} SELL: raw={:.10f} -> rounded={:.10f} (sizer={})",
                     symbol, rawQty, orderQty, hasSizer);
            currentAmount = orderQty * orderPrice * feeMult;
        }

        if (orderQty <= 0) {
            failReason = "leg" + std::to_string(leg) + ":" + symbol + " qty<=0";
            break;
        }

        order.setPrice(orderPrice);
        order.setQty(orderQty);
        order.setType(OrderType::MARKET);
    }

    const double pnl = currentAmount - initialStake;

    if (!failReason.empty()) {
        LOG_DEBUG("[Validate] {} FAIL: {}", description(), failReason);
        return std::nullopt;
    }

    if (pnl > 0) {
        LOG_DEBUG("[Validate] {} OK: pnl={:.6f} ({:+.4f}%)", description(), pnl, (pnl / initialStake) * 100.0);
        return Signal(workingOrders, description(), pnl);
    }

    LOG_DEBUG("[Validate] {} FAIL: pnl={:.6f} not profitable", description(), pnl);
    return std::nullopt;
}