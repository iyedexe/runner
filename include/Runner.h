#pragma once

#include <memory>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>

// Exception thrown when arbitrage execution fails critically
// (rejection, partial fill, or any order failure)
class ArbitrageExecutionError : public std::runtime_error {
public:
    ArbitrageExecutionError(const std::string& msg, int legIndex, const std::string& clOrdId)
        : std::runtime_error(msg), legIndex_(legIndex), clOrdId_(clOrdId) {}

    int legIndex() const { return legIndex_; }
    const std::string& clOrdId() const { return clOrdId_; }

private:
    int legIndex_;
    std::string clOrdId_;
};

#include "market_connection/Admin.h"
#include "market_connection/Feeder.h"
#include "market_connection/Broker.h"
#include "crypto/ed25519.hpp"

#include "strategies/TriangularArbitrage.h"
#include "fin/SymbolFilters.h"
#include "fin/OrderSizer.h"
#include "fin/Symbol.h"

struct RunnerConfig {
    // FIX connection settings
    std::string fixMdEndpoint;
    int fixMdPort = 9000;
    std::string fixOeEndpoint;
    int fixOePort = 9000;

    // REST API settings
    std::string restEndpoint = "testnet.binance.vision";

    // Authentication
    std::string apiKey;
    std::string ed25519KeyPath;

    // Execution settings
    bool liveMode = false;

    // Strategy config (nested)
    TriangularArbitrageConfig strategyConfig;
};

class Runner {
public:
    explicit Runner(const RunnerConfig& config);
    ~Runner() = default;

    void initialize();
    void shutdown();
    void run();

    static RunnerConfig loadConfig(const std::string& configFile);

private:
    RunnerConfig config_;

    // Infrastructure - market connection layer
    std::unique_ptr<crypto::ed25519> key_;
    std::unique_ptr<Admin> admin_;      // REST-based operations
    std::unique_ptr<Feeder> feeder_;    // FIX market data
    std::unique_ptr<Broker> broker_;    // FIX order execution

    // Strategy
    std::unique_ptr<TriangularArbitrage> strategy_;

    // State
    std::map<std::string, double> balance_;
    std::vector<fin::Symbol> symbolsList_;
    OrderSizer orderSizer_;

    // Wait for market data snapshots
    void waitForMarketDataSnapshots();

    // Order execution
    void executeArbitrage(const Signal& signal);
};
