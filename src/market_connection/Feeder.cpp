#include "market_connection/Feeder.h"
#include "fix/messages/InstrumentList.hpp"
#include "fix/messages/MarketDataRequest.hpp"
#include "fix/parsers/InstrumentListParser.hpp"
#include "fix/parsers/MarketDataParser.hpp"
#include "logger.hpp"

Feeder::Feeder(const std::string& apiKey, crypto::ed25519& key, OrderBook& orderBook)
    : BNB::FIX::Feeder(apiKey, key)
    , orderBook_(orderBook)
    , instrumentListFuture_(instrumentListPromise_.get_future())
{
}

SymbolId Feeder::getOrCreateSymbolId(const std::string& symbol) {
    // Fast path: check cache
    {
        std::lock_guard<std::mutex> lock(symbolIdCacheMtx_);
        auto it = symbolIdCache_.find(symbol);
        if (it != symbolIdCache_.end()) {
            return it->second;
        }
    }

    // Slow path: register and cache
    SymbolId id = SymbolRegistry::instance().registerSymbol(symbol);

    {
        std::lock_guard<std::mutex> lock(symbolIdCacheMtx_);
        symbolIdCache_[symbol] = id;
    }

    return id;
}

void Feeder::subscribeToSymbols(const std::vector<std::string>& symbols) {
    if (symbols.empty()) {
        LOG_WARNING("[Feeder] No symbols to subscribe to");
        return;
    }

    LOG_INFO("[Feeder] Subscribing to {} symbols", symbols.size());

    // Pre-register all symbols in registry and cache IDs
    for (const auto& symbol : symbols) {
        getOrCreateSymbolId(symbol);
    }

    setExpectedSymbols(symbols);
    std::string reqId = "mdReq" + std::to_string(++mdReqIdCounter_);

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

    std::string reqIdToUnsubscribe;
    {
        std::lock_guard<std::mutex> lock(subscriptionMtx_);
        for (const auto& [reqId, subSymbols] : subscriptionSymbols_) {
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
        LOG_WARNING("[Feeder] No active subscription found for symbols");
        return;
    }

    MarketDataRequest request(reqIdToUnsubscribe, SubscriptionAction::Unsubscribe);
    request.setMarketDepth(1);
    sendMessage(request);

    {
        std::lock_guard<std::mutex> lock(subscriptionMtx_);
        subscriptionSymbols_.erase(reqIdToUnsubscribe);
    }
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
    auto update = BNB::FIX::MarketDataParser::parseSnapshot(message);

    LOG_DEBUG("[Feeder] Received snapshot for {}: bid={}, ask={}",
              update.symbol, update.bestBidPrice, update.bestAskPrice);

    // Get symbol ID (should be cached from subscription)
    SymbolId symbolId = getOrCreateSymbolId(update.symbol);

    // Update lock-free order book using SymbolId
    orderBook_.update(symbolId, update.bestBidPrice, update.bestAskPrice);

    // Track snapshot receipt
    bool allReceived = false;
    {
        std::lock_guard<std::mutex> lock(snapshotMtx_);
        if (expectedSymbols_.find(update.symbol) != expectedSymbols_.end()) {
            receivedSnapshots_.insert(update.symbol);
            allReceived = (receivedSnapshots_.size() >= expectedSymbols_.size());
        }
    }
    if (allReceived) {
        snapshotCv_.notify_all();
    }
}

void Feeder::onMessage(const FIX44::MD::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) {
    auto updates = BNB::FIX::MarketDataParser::parseIncrementalRefresh(message);

    for (const auto& update : updates) {
        LOG_DEBUG("[Feeder] Received update for {}: bid={}, ask={}",
                  update.symbol, update.bestBidPrice, update.bestAskPrice);

        // Get symbol ID (should be cached)
        SymbolId symbolId = getOrCreateSymbolId(update.symbol);

        // Update lock-free order book using SymbolId
        orderBook_.update(symbolId, update.bestBidPrice, update.bestAskPrice);
    }
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

void Feeder::setExpectedSymbols(const std::vector<std::string>& symbols) {
    std::lock_guard<std::mutex> lock(snapshotMtx_);
    expectedSymbols_.clear();
    receivedSnapshots_.clear();
    for (const auto& sym : symbols) {
        expectedSymbols_.insert(sym);
    }
}

bool Feeder::waitForAllSnapshots(int timeoutMs) {
    std::unique_lock<std::mutex> lock(snapshotMtx_);
    return snapshotCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return expectedSymbols_.empty() || receivedSnapshots_.size() >= expectedSymbols_.size();
    });
}

std::pair<size_t, size_t> Feeder::getSnapshotProgress() const {
    std::lock_guard<std::mutex> lock(snapshotMtx_);
    return {receivedSnapshots_.size(), expectedSymbols_.size()};
}
