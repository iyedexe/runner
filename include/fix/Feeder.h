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

    MarketData getUpdate();
    bool hasUpdate() const;

    std::vector<SymbolInfo> getSymbols();
    void waitForInstrumentList();

protected:
    void onMessage(const FIX44::MD::InstrumentList& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataSnapshot& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MD::MarketDataRequestReject& message, const FIX::SessionID& sessionID) override;

private:
    void queueMarketData(const MarketData& data);

    std::queue<MarketData> updateQueue_;
    mutable std::mutex queueMtx_;
    std::condition_variable queueCv_;

    std::vector<SymbolInfo> symbols_;
    mutable std::mutex symbolsMtx_;

    std::promise<void> instrumentListPromise_;
    std::shared_future<void> instrumentListFuture_;
    std::atomic<bool> instrumentListReceived_{false};

    int mdReqIdCounter_ = 0;
};

} // namespace TriArb
