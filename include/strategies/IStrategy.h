#pragma once
#include <string>
#include <optional>
#include "fin/Signal.h"

struct MarketData {
    std::string symbol;
    double bestBidPrice = 0.0;
    double bestBidQty = 0.0;
    double bestAskPrice = 0.0;
    double bestAskQty = 0.0;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual std::optional<Signal> onMarketData(const MarketData& data) = 0;

    virtual void initialize() = 0;

    virtual void shutdown() = 0;

    virtual void run() = 0;
};
