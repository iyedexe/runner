#pragma once

#include <fix/Broker.hpp>
#include <fix/types/OrderTypes.hpp>
#include <map>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace TriArb {

// Use libxchange OrderStatus type
using OrderStatus = BNB::FIX::OrderStatus;

struct OrderState {
    std::string clOrdId;
    std::string orderId;
    std::string symbol;
    char side;
    double orderQty = 0.0;
    double cumQty = 0.0;
    double avgPx = 0.0;
    OrderStatus status = OrderStatus::UNKNOWN;
    std::string rejectReason;
};

class Broker : public BNB::FIX::Broker {
public:
    Broker(const std::string& apiKey, crypto::ed25519& key, bool liveMode = false);
    virtual ~Broker() = default;

    std::string sendMarketOrder(const std::string& symbol, char side, double qty);
    std::string testMarketOrder(const std::string& symbol, char side, double qty);

    OrderState getOrderState(const std::string& clOrdId);
    OrderStatus waitForOrderCompletion(const std::string& clOrdId, int timeoutMs = 5000);

    bool isLiveMode() const { return liveMode_; }
    void setLiveMode(bool live) { liveMode_ = live; }

protected:
    void onMessage(const FIX44::OE::ExecutionReport& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::OE::OrderCancelReject& message, const FIX::SessionID& sessionID) override;

private:
    std::string generateClOrdId();

    std::map<std::string, OrderState> orderStates_;
    mutable std::mutex orderMtx_;
    std::condition_variable orderCv_;

    std::atomic<int> orderIdCounter_{0};
    bool liveMode_ = false;
};

} // namespace TriArb
