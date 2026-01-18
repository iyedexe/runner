#include "fix/Feeder.h"
#include "fix/messages/InstrumentList.hpp"
#include "fix/messages/MarketDataRequest.hpp"
#include "fix/parsers/InstrumentListParser.hpp"
#include "fix/parsers/MarketDataParser.hpp"
#include "common/logger.hpp"

namespace TriArb {

Feeder::Feeder(const std::string& apiKey, crypto::ed25519& key)
    : BNB::FIX::Feeder(apiKey, key)
    , instrumentListFuture_(instrumentListPromise_.get_future())
{
}

void Feeder::requestInstrumentList() {
    LOG_INFO("[Feeder] Requesting instrument list");
    InstrumentList request("instrReq1");
    sendMessage(request);
}

void Feeder::subscribeToSymbols(const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        LOG_WARNING("[Feeder] No symbols to subscribe to");
        return;
    }

    LOG_INFO("[Feeder] Subscribing to {} symbols", symbols.size());

    std::string reqId = "mdReq" + std::to_string(++mdReqIdCounter_);
    MarketDataRequest request(reqId);
    request.subscribeToStream(StreamType::BookTicker);
    request.setMarketDepth(1);

    for (const auto& symbol : symbols) {
        request.forSymbol(symbol);
    }

    sendMessage(request);
}

void Feeder::unsubscribeFromSymbols(const std::vector<std::string>& symbols) {
    LOG_INFO("[Feeder] Unsubscribing from {} symbols", symbols.size());
    // TODO: Implement unsubscribe using SubscriptionRequestType_UNSUBSCRIBE
}

MarketData Feeder::getUpdate() {
    std::unique_lock<std::mutex> lock(queueMtx_);
    queueCv_.wait(lock, [this] { return !updateQueue_.empty(); });

    MarketData data = updateQueue_.front();
    updateQueue_.pop();
    return data;
}

bool Feeder::hasUpdate() const {
    std::lock_guard<std::mutex> lock(queueMtx_);
    return !updateQueue_.empty();
}

std::vector<SymbolInfo> Feeder::getSymbols() {
    std::lock_guard<std::mutex> lock(symbolsMtx_);
    return symbols_;
}

void Feeder::waitForInstrumentList() {
    if (!instrumentListReceived_) {
        instrumentListFuture_.wait();
    }
}

void Feeder::onMessage(const FIX44::MD::InstrumentList& message, const FIX::SessionID& sessionID) {
    LOG_INFO("[Feeder] Received InstrumentList");

    // Use libxchange parser
    auto parsedSymbols = BNB::FIX::InstrumentListParser::parse(message);

    {
        std::lock_guard<std::mutex> lock(symbolsMtx_);
        symbols_ = parsedSymbols;
    }

    LOG_INFO("[Feeder] Parsed {} symbols", parsedSymbols.size());

    if (!instrumentListReceived_.exchange(true)) {
        instrumentListPromise_.set_value();
    }
}

void Feeder::onMessage(const FIX44::MD::MarketDataSnapshot& message, const FIX::SessionID& sessionID) {
    // Use libxchange parser
    auto update = BNB::FIX::MarketDataParser::parseSnapshot(message);

    // Convert to runner's MarketData type
    MarketData data;
    data.symbol = update.symbol;
    data.bestBidPrice = update.bestBidPrice;
    data.bestBidQty = update.bestBidQty;
    data.bestAskPrice = update.bestAskPrice;
    data.bestAskQty = update.bestAskQty;

    queueMarketData(data);
}

void Feeder::onMessage(const FIX44::MD::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) {
    // Use libxchange parser
    auto updates = BNB::FIX::MarketDataParser::parseIncrementalRefresh(message);

    // Convert to runner's MarketData type and queue all updates
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        for (const auto& update : updates) {
            MarketData data;
            data.symbol = update.symbol;
            data.bestBidPrice = update.bestBidPrice;
            data.bestBidQty = update.bestBidQty;
            data.bestAskPrice = update.bestAskPrice;
            data.bestAskQty = update.bestAskQty;
            updateQueue_.push(data);
        }
    }
    queueCv_.notify_all();
}

void Feeder::onMessage(const FIX44::MD::MarketDataRequestReject& message, const FIX::SessionID& sessionID) {
    FIX::MD::MDReqID reqId;
    message.get(reqId);

    FIX::MD::Text text;
    std::string reason;
    if (message.isSet(text)) {
        message.get(text);
        reason = text.getValue();
    }

    LOG_ERROR("[Feeder] MarketDataRequest rejected: reqId={}, reason={}", reqId.getValue(), reason);
}

void Feeder::queueMarketData(const MarketData& data) {
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        updateQueue_.push(data);
    }
    queueCv_.notify_one();
}

} // namespace TriArb
