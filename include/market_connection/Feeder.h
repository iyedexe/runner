#pragma once

#include <fix/Feeder.hpp>
#include <fix/types/MarketDataTypes.hpp>
#include <unordered_map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <future>
#include <atomic>

#include "fin/Symbol.h"
#include "market_connection/OrderBook.h"

// Use libxchange SymbolInfo type
using SymbolInfo = BNB::FIX::SymbolInfo;

/**
 * Feeder - FIX market data handler with high-performance OrderBook.
 *
 * Writes to lock-free OrderBook using SymbolId for O(1) updates.
 */
class Feeder : public BNB::FIX::Feeder {
public:
    Feeder(const std::string& apiKey, crypto::ed25519& key, OrderBook& orderBook);
    virtual ~Feeder() = default;

    void subscribeToSymbols(const std::vector<std::string>& symbols);
    void unsubscribeFromSymbols(const std::vector<std::string>& symbols);

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
    OrderBook& orderBook_;

    // Pre-computed symbol ID cache for O(1) lookup in hot path
    std::unordered_map<std::string, SymbolId> symbolIdCache_;
    mutable std::mutex symbolIdCacheMtx_;

    // Snapshot tracking for initialization
    std::set<std::string> expectedSymbols_;
    std::set<std::string> receivedSnapshots_;
    mutable std::mutex snapshotMtx_;
    std::condition_variable snapshotCv_;

    std::vector<SymbolInfo> symbols_;
    mutable std::mutex symbolsMtx_;

    std::promise<void> instrumentListPromise_;
    std::shared_future<void> instrumentListFuture_;
    std::atomic<bool> instrumentListReceived_{false};

    int mdReqIdCounter_ = 0;

    std::map<std::string, std::vector<std::string>> subscriptionSymbols_;
    mutable std::mutex subscriptionMtx_;

    // Get or create symbol ID (with caching)
    SymbolId getOrCreateSymbolId(const std::string& symbol);
};
