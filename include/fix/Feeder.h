#pragma once

#include <fix/Feeder.hpp>
#include <fix/types/MarketDataTypes.hpp>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <future>
#include <atomic>
#include "strategies/IStrategy.h"
#include "fin/Symbol.h"
#include "fix/MarketDataStore.h"

namespace TriArb {

// Use libxchange SymbolInfo type
using SymbolInfo = BNB::FIX::SymbolInfo;

class Feeder : public BNB::FIX::Feeder {
public:
    Feeder(const std::string& apiKey, crypto::ed25519& key);
    virtual ~Feeder() = default;

    void requestInstrumentList();
    void subscribeToSymbols(const std::vector<std::string>& symbols);
    void unsubscribeFromSymbols(const std::vector<std::string>& symbols);

    // Queue-based update retrieval (blocking)
    MarketData getUpdate();
    bool hasUpdate() const;

    // Direct access to market data store
    MarketDataStore& getMarketDataStore() { return marketDataStore_; }
    const MarketDataStore& getMarketDataStore() const { return marketDataStore_; }

    std::vector<SymbolInfo> getSymbols();
    void waitForInstrumentList();

protected:
    void onMessage(const FIX44::MD::InstrumentList& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataSnapshot& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataRequestReject& message, const FIX::SessionID& sessionID) override;

private:
    void queueMarketData(const MarketData& data);

    // Market data storage for best bid/ask
    MarketDataStore marketDataStore_;

    // Queue for strategy processing
    std::queue<MarketData> updateQueue_;
    mutable std::mutex queueMtx_;
    std::condition_variable queueCv_;

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

} // namespace TriArb
