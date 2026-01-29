#pragma once

#include <memory>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include "market_connection/Admin.h"
#include "market_connection/Feeder.h"
#include "market_connection/Broker.h"
#include "market_connection/OrderBook.h"
#include "crypto/ed25519.hpp"

#include "strategies/TriangularArbitrage.h"
#include "fin/SymbolFilters.h"
#include "fin/OrderSizer.h"
#include "fin/Symbol.h"

// Exception thrown when arbitrage execution fails mid-way
class ArbitrageExecutionError : public std::runtime_error {
public:
    ArbitrageExecutionError(const std::string& msg, int failedLeg, const std::string& orderId)
        : std::runtime_error(msg), failedLeg_(failedLeg), orderId_(orderId) {}

    int failedLeg() const { return failedLeg_; }
    const std::string& orderId() const { return orderId_; }

private:
    int failedLeg_;
    std::string orderId_;
};

/**
 * Polling mode for the main loop.
 */
enum class PollingMode {
    Blocking,     // Use condition variable (lower CPU, higher latency)
    BusyPoll,     // Spin with pause hints (higher CPU, lower latency)
    Hybrid        // Spin for N iterations, then block
};

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
    PollingMode pollingMode = PollingMode::Hybrid;
    int busyPollSpinCount = 10000;

    // Strategy config (nested)
    TriangularArbitrageConfig strategyConfig;
};

/**
 * Runner - High-performance trading orchestrator.
 *
 * Optimizations:
 * 1. Lock-free OrderBook with seqlock
 * 2. Integer symbol IDs for O(1) lookups
 * 3. Inverted index for fast path filtering
 * 4. Configurable polling mode
 */
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
    std::unique_ptr<Admin> admin_;
    OrderBook orderBook_;
    std::unique_ptr<Feeder> feeder_;
    std::unique_ptr<Broker> broker_;

    // Strategy
    std::unique_ptr<TriangularArbitrage> strategy_;

    // State
    std::map<std::string, double> balance_;
    std::vector<fin::Symbol> symbolsList_;
    OrderSizer orderSizer_;

    void waitForMarketDataSnapshots();
    void executeArbitrage(const Signal& signal);

    // Execution result tracking
    struct LegResult {
        std::string symbol;
        Way way;
        double estPrice;
        double realPrice;
        double estQty;
        double realQty;
        double feeRate;
    };

    void handleExecutionFailure(int legIndex, const std::string& clOrdId, const std::string& reason);
};
