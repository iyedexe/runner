#include "market_connection/Feeder.h"
#include "fix/messages/InstrumentList.hpp"
#include "fix/messages/MarketDataRequest.hpp"
#include "fix/parsers/InstrumentListParser.hpp"
#include "fix/parsers/MarketDataParser.hpp"
#include "logger.hpp"

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
    setExpectedSymbols(symbols);
    std::string reqId = "mdReq" + std::to_string(++mdReqIdCounter_);

    // Track which symbols are associated with this subscription request
    {
        std::lock_guard<std::mutex> lock(subscriptionMtx_);
        subscriptionSymbols_[reqId] = symbols;
    }

    MarketDataRequest request(reqId, SubscriptionAction::Subscribe);
    request.subscribeToStream(StreamType::BookTicker);
    request.setMarketDepth(1);

    for (const auto& symbol : symbols) {
        request.forSymbol(symbol);
    }

    sendMessage(request);
}

void Feeder::unsubscribeFromSymbols(const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        LOG_WARNING("[Feeder] No symbols to unsubscribe from");
        return;
    }

    LOG_INFO("[Feeder] Unsubscribing from {} symbols", symbols.size());

    // Find the request ID associated with these symbols
    std::string reqIdToUnsubscribe;
    {
        std::lock_guard<std::mutex> lock(subscriptionMtx_);
        for (const auto& [reqId, subSymbols] : subscriptionSymbols_) {
            // Check if any of the symbols to unsubscribe match
            for (const auto& sym : symbols) {
                if (std::find(subSymbols.begin(), subSymbols.end(), sym) != subSymbols.end()) {
                    reqIdToUnsubscribe = reqId;
                    break;
                }
            }
            if (!reqIdToUnsubscribe.empty()) break;
        }
    }

    if (reqIdToUnsubscribe.empty()) {
        LOG_WARNING("[Feeder] No active subscription found for symbols to unsubscribe");
        return;
    }

    // Send unsubscribe request with the same reqId
    MarketDataRequest request(reqIdToUnsubscribe, SubscriptionAction::Unsubscribe);
    request.setMarketDepth(1);  // Required for unsubscribe
    sendMessage(request);

    // Remove from tracking
    {
        std::lock_guard<std::mutex> lock(subscriptionMtx_);
        subscriptionSymbols_.erase(reqIdToUnsubscribe);
    }
}


std::unordered_map<std::string, BidAsk> Feeder::waitForBookUpdate() {
    std::unique_lock affectedLock(affectedMtx_);
    affectedCv_.wait(affectedLock, [this] {
        return !affectedSymbols_.empty();
    });

    std::unordered_map<std::string, BidAsk> result;

    {
        std::lock_guard priceLock(pricesMtx_);
        for (const auto& symbol : affectedSymbols_) {
            auto it = prices_.find(symbol);
            if (it != prices_.end()) {
                result[symbol] = it->second;
            }
        }
    }

    affectedSymbols_.clear();
    return result;
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

    LOG_DEBUG("[Feeder] Received snapshot for {}: bid={}, ask={}",
              update.symbol, update.bestBidPrice, update.bestAskPrice);

    // Update price storage
    updatePrice(update.symbol, update.bestBidPrice, update.bestAskPrice);

    // Track snapshot receipt
    bool allReceived = false;
    {
        std::lock_guard<std::mutex> lock(pricesMtx_);
        if (expectedSymbols_.find(update.symbol) != expectedSymbols_.end()) {
            receivedSnapshots_.insert(update.symbol);
            allReceived = (receivedSnapshots_.size() >= expectedSymbols_.size());
        }
    }
    if (allReceived) {
        snapshotCv_.notify_all();
    }

    // Track affected symbol
    {
        std::lock_guard<std::mutex> lock(affectedMtx_);
        affectedSymbols_.insert(update.symbol);
    }
    affectedCv_.notify_one();
}

void Feeder::onMessage(const FIX44::MD::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) {
    // Use libxchange parser
    auto updates = BNB::FIX::MarketDataParser::parseIncrementalRefresh(message);

    // Process each update
    {
        std::lock_guard<std::mutex> lock(pricesMtx_);
        for (const auto& update : updates) {
            LOG_DEBUG("[Feeder] Received update for {}: bid={}, ask={}",
                      update.symbol, update.bestBidPrice, update.bestAskPrice);

            auto& price = prices_[update.symbol];
            if (update.bestBidPrice > 0) price.bid = update.bestBidPrice;
            if (update.bestAskPrice > 0) price.ask = update.bestAskPrice;
        }
    }
    priceVersion_.fetch_add(1, std::memory_order_release);

    // Track affected symbols (deduplicated via set)
    {
        std::lock_guard<std::mutex> lock(affectedMtx_);
        for (const auto& update : updates) {
            affectedSymbols_.insert(update.symbol);
        }
    }
    affectedCv_.notify_one();
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

void Feeder::updatePrice(const std::string& symbol, double bid, double ask) {
    {
        std::lock_guard<std::mutex> lock(pricesMtx_);
        auto& price = prices_[symbol];
        if (bid > 0) price.bid = bid;
        if (ask > 0) price.ask = ask;
    }
    priceVersion_.fetch_add(1, std::memory_order_release);
}

BidAsk Feeder::getPrice(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(pricesMtx_);
    auto it = prices_.find(symbol);
    if (it != prices_.end()) {
        return it->second;
    }
    return {0.0, 0.0};
}

void Feeder::setExpectedSymbols(const std::vector<std::string>& symbols) {
    std::lock_guard<std::mutex> lock(pricesMtx_);
    expectedSymbols_.clear();
    receivedSnapshots_.clear();
    for (const auto& sym : symbols) {
        expectedSymbols_.insert(sym);
    }
}

bool Feeder::waitForAllSnapshots(int timeoutMs) {
    std::unique_lock<std::mutex> lock(pricesMtx_);
    return snapshotCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return expectedSymbols_.empty() || receivedSnapshots_.size() >= expectedSymbols_.size();
    });
}

std::pair<size_t, size_t> Feeder::getSnapshotProgress() const {
    std::lock_guard<std::mutex> lock(pricesMtx_);
    return {receivedSnapshots_.size(), expectedSymbols_.size()};
}
