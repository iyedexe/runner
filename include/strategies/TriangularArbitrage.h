#pragma once

#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <optional>

#include "IStrategy.h"
#include "strategies/ArbitragePath.h"
#include "fin/Symbol.h"
#include "fin/Signal.h"
#include "fin/OrderSizer.h"

struct TriangularArbitrageConfig {
    std::string startingAsset;
    double defaultFee = 0.1;
    double risk = 1.0;
    std::map<std::string, double> symbolFees;
};

class TriangularArbitrage : public IStrategy {
public:
    explicit TriangularArbitrage(const TriangularArbitrageConfig& config);
    virtual ~TriangularArbitrage() = default;

    void discoverRoutes(const std::vector<fin::Symbol>& symbols) override;
    const std::set<std::string>& subscribedSymbols() const override { return stratSymbols_; }
    std::optional<Signal> onMarketDataBatch(
        const std::unordered_map<std::string, BidAsk>& prices,
        double stake,
        const OrderSizer& sizer) override;

    const std::string& startingAsset() const { return startingAsset_; }
    double risk() const { return risk_; }
    double getFeeForSymbol(const std::string& symbol) const;

private:
    std::string startingAsset_;
    double defaultFee_;
    double risk_;
    std::map<std::string, double> symbolFees_;

    std::set<std::string> stratSymbols_;
    std::vector<std::shared_ptr<ArbitragePath>> paths_;

    std::vector<Order> getPossibleOrders(const std::string& coin, const std::vector<fin::Symbol>& relatedSymbols);
    std::vector<ArbitragePath> computeArbitragePaths(
        const std::vector<fin::Symbol>& symbolsList,
        const std::string& startingAsset,
        int arbitrageDepth);

    static constexpr size_t TOP_K_CANDIDATES = 5;
};
