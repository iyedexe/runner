#include "market_connection/Broker.h"
#include "fix/messages/NewSingleOrder.hpp"
#include "fix/parsers/ExecutionReportParser.hpp"
#include "codegen/fix/OE/FixValues.h"
#include "logger.hpp"
#include <chrono>

Broker::Broker(const std::string& apiKey, crypto::ed25519& key, bool liveMode)
    : BNB::FIX::Broker(apiKey, key)
    , liveMode_(liveMode)
{
}

std::string Broker::sendMarketOrder(const std::string& symbol, char side, double qty, double estPrice) {
    std::string clOrdId = generateClOrdId();

    LOG_INFO("[Broker] Sending market order: clOrdId={}, symbol={}, side={}, qty={:.8f}",
             clOrdId, symbol, side, qty);

    if (!liveMode_) {
        LOG_WARNING("[Broker] Test mode - order not sent to exchange");
        return testMarketOrder(symbol, side, qty, estPrice);
    }

    // Create order state before sending
    {
        std::lock_guard<std::mutex> lock(orderMtx_);
        OrderState state;
        state.clOrdId = clOrdId;
        state.symbol = symbol;
        state.side = side;
        state.orderQty = qty;
        state.status = OrderStatus::PENDING_NEW;
        orderStates_[clOrdId] = state;
    }

    NewSingleOrder order(clOrdId, FIX::OE::OrdType_MARKET, side, symbol);
    order.orderQty(qty);

    sendMessage(order);

    return clOrdId;
}

std::string Broker::testMarketOrder(const std::string& symbol, char side, double qty, double estPrice) {
    std::string clOrdId = generateClOrdId();

    LOG_INFO("[Broker] Test market order: clOrdId={}, symbol={}, side={}, qty={}, estPrice={}",
             clOrdId, symbol, side, qty, estPrice);

    // Simulate immediate fill in test mode using estimated price
    {
        std::lock_guard<std::mutex> lock(orderMtx_);
        OrderState state;
        state.clOrdId = clOrdId;
        state.symbol = symbol;
        state.side = side;
        state.orderQty = qty;
        state.cumQty = qty;
        state.avgPx = estPrice;  // Use estimated price for test mode
        state.status = OrderStatus::FILLED;
        orderStates_[clOrdId] = state;
    }
    orderCv_.notify_all();

    return clOrdId;
}

OrderState Broker::getOrderState(const std::string& clOrdId) {
    std::lock_guard<std::mutex> lock(orderMtx_);
    auto it = orderStates_.find(clOrdId);
    if (it != orderStates_.end()) {
        return it->second;
    }
    return OrderState{};
}

OrderStatus Broker::waitForOrderCompletion(const std::string& clOrdId, int timeoutMs) {
    std::unique_lock<std::mutex> lock(orderMtx_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (true) {
        auto it = orderStates_.find(clOrdId);
        if (it != orderStates_.end()) {
            OrderStatus status = it->second.status;
            if (status == OrderStatus::FILLED ||
                status == OrderStatus::CANCELED ||
                status == OrderStatus::REJECTED ||
                status == OrderStatus::EXPIRED) {
                return status;
            }
        }

        if (orderCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            LOG_WARNING("[Broker] Timeout waiting for order completion: {}", clOrdId);
            return OrderStatus::UNKNOWN;
        }
    }
}

void Broker::onMessage(const FIX44::OE::ExecutionReport& message, const FIX::SessionID& sessionID) {
    // Use libxchange parser to extract fields
    auto exec = BNB::FIX::ExecutionReportParser::parse(message);

    LOG_INFO("[Broker] ExecutionReport: clOrdId={}, symbol={}, execType={}, ordStatus={}, cumQty={}, lastPx={}, lastQty={}",
             exec.clOrdId, exec.symbol, static_cast<int>(exec.execType), static_cast<int>(exec.status),
             exec.cumQty, exec.lastPx, exec.lastQty);

    {
        std::lock_guard<std::mutex> lock(orderMtx_);
        auto it = orderStates_.find(exec.clOrdId);
        if (it == orderStates_.end()) {
            // New order we haven't seen before
            OrderState state;
            state.clOrdId = exec.clOrdId;
            orderStates_[exec.clOrdId] = state;
            it = orderStates_.find(exec.clOrdId);
        }

        it->second.orderId = exec.orderId;
        it->second.symbol = exec.symbol;
        it->second.side = BNB::FIX::sideToChar(exec.side);
        it->second.orderQty = exec.orderQty;
        it->second.cumQty = exec.cumQty;
        it->second.status = exec.status;
        it->second.rejectReason = exec.text;

        // Calculate avgPx from fills (for TRADE exec type)
        if (exec.execType == BNB::FIX::ExecType::TRADE && exec.lastQty > 0) {
            it->second.cumCost += exec.lastPx * exec.lastQty;
            if (it->second.cumQty > 0) {
                it->second.avgPx = it->second.cumCost / it->second.cumQty;
            }
            LOG_INFO("[Broker] Fill: lastPx={:.8f}, lastQty={:.8f}, avgPx={:.8f}",
                     exec.lastPx, exec.lastQty, it->second.avgPx);
        }
    }
    orderCv_.notify_all();
}

void Broker::onMessage(const FIX44::OE::OrderCancelReject& message, const FIX::SessionID& sessionID) {
    FIX::OE::ClOrdID clOrdIdField;
    message.get(clOrdIdField);
    std::string clOrdId = clOrdIdField.getValue();

    std::string reason;
    FIX::OE::Text textField;
    if (message.isSet(textField)) {
        message.get(textField);
        reason = textField.getValue();
    }

    LOG_ERROR("[Broker] OrderCancelReject: clOrdId={}, reason={}", clOrdId, reason);
}

std::string Broker::generateClOrdId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return "TA" + std::to_string(ms) + "_" + std::to_string(++orderIdCounter_);
}
