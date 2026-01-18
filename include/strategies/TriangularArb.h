#pragma once

#include <vector>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <stdexcept>
#include <numeric>
#include <algorithm>
#include <iostream>
#include <charconv>
#include <cmath>

// Include fix headers first (they include libxchange logger)
#include "fix/Feeder.h"
#include "fix/Broker.h"

#include "IStrategy.h"
#include "fin/SymbolFilter.h"
#include "fin/Order.h"
#include "fin/Symbol.h"
#include "fin/Signal.h"

#include "crypto/ed25519.hpp"

struct TriangularArbConfig {
    std::string startingAsset;
    std::string fixMdEndpoint;
    int fixMdPort = 9000;
    std::string fixOeEndpoint;
    int fixOePort = 9000;
    std::string apiKey;
    std::string ed25519KeyPath;
    double defaultFee = 0.1;  // Default fee percentage for symbols not in symbolFees
    std::map<std::string, double> symbolFees;  // Per-symbol fee percentages
    double risk = 1.0;
    bool liveMode = false;
};

class TriangularArb : public IStrategy {
public:
    TriangularArb(const TriangularArbConfig& config);
    virtual ~TriangularArb() = default;

    std::optional<Signal> onMarketData(const MarketData& data) override;
    void initialize() override;
    void shutdown() override;
    void run() override;

    static TriangularArbConfig loadConfig(const std::string& configFile);

private:
    TriangularArbConfig config_;
    std::unique_ptr<crypto::ed25519> key_;
    std::unique_ptr<TriArb::Feeder> feeder_;
    std::unique_ptr<TriArb::Broker> broker_;

    std::string startingAsset_;
    std::vector<std::vector<Order>> stratPaths_;
    std::set<std::string> stratSymbols_;
    std::map<std::string, MarketData> marketData_;
    std::map<std::string, double> balance_;
    std::vector<::Symbol> symbolsList_;

    double defaultFee_;
    std::map<std::string, double> symbolFees_;
    double risk_;

    double getFeeForSymbol(const std::string& symbol) const;

    void discoverArbitrageRoutes();
    std::vector<Order> getPossibleOrders(const std::string& coin, const std::vector<::Symbol>& relatedSymbols);
    std::vector<std::vector<Order>> computeArbitragePaths(const std::vector<::Symbol>& symbolsList, const std::string& startingAsset, int arbitrageDepth);
    std::optional<Signal> evaluatePath(std::vector<Order>& path);
    void executeArbitrage(const Signal& signal);

    ::Symbol createSymbol(const TriArb::SymbolInfo& info);
};
