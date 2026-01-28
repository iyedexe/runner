#pragma once

#include <fix/Feeder.hpp>
#include <fix/types/MarketDataTypes.hpp>
#include <queue>
#include <unordered_map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <future>
#include <atomic>
#include "strategies/IStrategy.h"
#include "fin/Symbol.h"

// Use libxchange SymbolInfo type
using SymbolInfo = BNB::FIX::SymbolInfo;

// Feeder handles FIX-based market data:
// - Symbol subscriptions
// - Price updates (bid/ask)
// - Snapshot management
class Feeder : public BNB::FIX::Feeder {
public:
    Feeder(const std::string& apiKey, crypto::ed25519& key);
    virtual ~Feeder() = default;

    void requestInstrumentList();
    void subscribeToSymbols(const std::vector<std::string>& symbols);
    void unsubscribeFromSymbols(const std::vector<std::string>& symbols);

    std::unordered_map<std::string, BidAsk> waitForBookUpdate();

    // Direct price access - always returns latest values
    BidAsk getPrice(const std::string& symbol) const;
    const std::unordered_map<std::string, BidAsk>& prices() const { return prices_; }
    uint64_t priceVersion() const { return priceVersion_.load(std::memory_order_acquire); }

    // Snapshot management for initialization
    void setExpectedSymbols(const std::vector<std::string>& symbols);
    bool waitForAllSnapshots(int timeoutMs = 30000);
    std::pair<size_t, size_t> getSnapshotProgress() const;

    std::vector<SymbolInfo> getSymbols();
    void waitForInstrumentList();

protected:
    void onMessage(const FIX44::MD::InstrumentList& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataSnapshot& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataRequestReject& message, const FIX::SessionID& sessionID) override;

private:
    void updatePrice(const std::string& symbol, double bid, double ask);

    // Simple price storage: symbol -> {bid, ask}
    std::unordered_map<std::string, BidAsk> prices_;
    mutable std::mutex pricesMtx_;
    std::atomic<uint64_t> priceVersion_{0};

    // Snapshot tracking for initialization
    std::set<std::string> expectedSymbols_;
    std::set<std::string> receivedSnapshots_;
    std::condition_variable snapshotCv_;

    // Affected symbols tracking (deduplicated)
    std::set<std::string> affectedSymbols_;
    mutable std::mutex affectedMtx_;
    std::condition_variable affectedCv_;

    std::vector<SymbolInfo> symbols_;
    mutable std::mutex symbolsMtx_;

    std::promise<void> instrumentListPromise_;
    std::shared_future<void> instrumentListFuture_;
    std::atomic<bool> instrumentListReceived_{false};

    int mdReqIdCounter_ = 0;

    // Track subscription request IDs for proper handling
    std::map<std::string, std::vector<std::string>> subscriptionSymbols_;  // reqId -> symbols
    mutable std::mutex subscriptionMtx_;
};
