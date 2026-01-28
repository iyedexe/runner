#pragma once
#include <string>
#include <optional>
#include <unordered_map>
#include <set>
#include <vector>
#include "fin/Symbol.h"

// Forward declarations
class OrderSizer;
struct Signal;

// Simple bid/ask pair for price storage
struct BidAsk {
    double bid = 0.0;
    double ask = 0.0;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Initialize paths/routes from symbol list
    virtual void discoverRoutes(const std::vector<fin::Symbol>& symbols) = 0;

    // Returns symbols the strategy needs market data for
    virtual const std::set<std::string>& subscribedSymbols() const = 0;

    // Main strategy callback - receives direct refs (no copy):
    // - prices: full price map from feeder
    // - affectedSymbols: symbols that were updated in this batch
    // Returns a signal if an opportunity is found
    virtual std::optional<Signal> onMarketDataBatch(
        const std::unordered_map<std::string, BidAsk>& prices,
        double stake,
        const OrderSizer& sizer) = 0;
};
