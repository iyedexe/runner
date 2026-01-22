#pragma once

#include <string>
#include <map>
#include <set>
#include <mutex>
#include <functional>
#include <vector>
#include <condition_variable>
#include <chrono>
#include "strategies/IStrategy.h"

namespace TriArb {

/**
 * Thread-safe storage for market data (best bid/ask) for all subscribed symbols.
 * Provides callback mechanism to notify when data changes for strategy re-evaluation.
 * Supports tracking expected symbols and waiting for all snapshots to arrive.
 */
class MarketDataStore {
public:
    using UpdateCallback = std::function<void(const MarketData&)>;

    MarketDataStore() = default;
    ~MarketDataStore() = default;

    // Non-copyable
    MarketDataStore(const MarketDataStore&) = delete;
    MarketDataStore& operator=(const MarketDataStore&) = delete;

    /**
     * Register a callback to be notified when market data is updated.
     * The callback will be invoked with the updated MarketData for the symbol.
     */
    void setUpdateCallback(UpdateCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    /**
     * Set the expected symbols that we're waiting for snapshots.
     * Call this before subscribing to market data.
     */
    void setExpectedSymbols(const std::vector<std::string>& symbols) {
        std::lock_guard<std::mutex> lock(mutex_);
        expectedSymbols_.clear();
        for (const auto& sym : symbols) {
            expectedSymbols_.insert(sym);
        }
        receivedSnapshots_.clear();
    }

    /**
     * Check if all expected symbols have received their initial snapshot.
     */
    bool allSnapshotsReceived() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expectedSymbols_.empty()) {
            return true;  // No expectation set, consider ready
        }
        return receivedSnapshots_.size() >= expectedSymbols_.size();
    }

    /**
     * Wait until all expected symbols have received snapshots, with timeout.
     * Returns true if all snapshots received, false on timeout.
     */
    bool waitForAllSnapshots(int timeoutMs = 30000) {
        std::unique_lock<std::mutex> lock(mutex_);
        return snapshotCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            return expectedSymbols_.empty() || receivedSnapshots_.size() >= expectedSymbols_.size();
        });
    }

    /**
     * Get count of expected and received snapshots.
     */
    std::pair<size_t, size_t> getSnapshotProgress() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {receivedSnapshots_.size(), expectedSymbols_.size()};
    }

    /**
     * Update market data from a snapshot message.
     * This typically initializes the data for a symbol.
     */
    void onSnapshot(const MarketData& data) {
        UpdateCallback callbackCopy;
        bool shouldNotify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            data_[data.symbol] = data;
            callbackCopy = callback_;

            // Track that we received this snapshot
            if (expectedSymbols_.find(data.symbol) != expectedSymbols_.end()) {
                receivedSnapshots_.insert(data.symbol);
                shouldNotify = (receivedSnapshots_.size() >= expectedSymbols_.size());
            }
        }

        // Notify waiters if all snapshots received
        if (shouldNotify) {
            snapshotCv_.notify_all();
        }

        // Invoke callback outside of lock to avoid deadlocks
        if (callbackCopy) {
            callbackCopy(data);
        }
    }

    /**
     * Update market data from an incremental refresh message.
     * Updates the existing data for the symbol, merging with current state.
     */
    void onIncrementalUpdate(const MarketData& update) {
        MarketData mergedData;
        UpdateCallback callbackCopy;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = data_.find(update.symbol);
            if (it != data_.end()) {
                // Merge with existing data
                mergedData = it->second;

                // Only update fields that have valid values in the update
                if (update.bestBidPrice > 0) {
                    mergedData.bestBidPrice = update.bestBidPrice;
                    mergedData.bestBidQty = update.bestBidQty;
                }
                if (update.bestAskPrice > 0) {
                    mergedData.bestAskPrice = update.bestAskPrice;
                    mergedData.bestAskQty = update.bestAskQty;
                }
            } else {
                // New symbol, just use the update as-is
                mergedData = update;
            }

            data_[update.symbol] = mergedData;
            callbackCopy = callback_;
        }

        // Invoke callback outside of lock
        if (callbackCopy) {
            callbackCopy(mergedData);
        }
    }

    /**
     * Get the current market data for a symbol.
     * Returns empty MarketData if symbol not found.
     */
    MarketData get(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = data_.find(symbol);
        if (it != data_.end()) {
            return it->second;
        }
        return MarketData{symbol, 0, 0, 0, 0};
    }

    /**
     * Check if market data exists for a symbol.
     */
    bool has(const std::string& symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.find(symbol) != data_.end();
    }

    /**
     * Get all current market data.
     */
    std::map<std::string, MarketData> getAll() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_;
    }

    /**
     * Get a list of all symbols with data.
     */
    std::vector<std::string> getSymbols() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> symbols;
        symbols.reserve(data_.size());
        for (const auto& [symbol, _] : data_) {
            symbols.push_back(symbol);
        }
        return symbols;
    }

    /**
     * Clear all stored data.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.clear();
    }

    /**
     * Get the number of symbols stored.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, MarketData> data_;
    UpdateCallback callback_;

    // Snapshot tracking
    std::set<std::string> expectedSymbols_;
    std::set<std::string> receivedSnapshots_;
    std::condition_variable snapshotCv_;
};

} // namespace TriArb
