#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include "strategies/IStrategy.h"

namespace TriArb {

/**
 * Thread-safe buffer that coalesces market data updates.
 * When multiple updates arrive for the same symbol before processing,
 * only the latest update is retained. This prevents redundant re-evaluations
 * during high-frequency market data scenarios.
 */
class CoalescingBuffer {
public:
    CoalescingBuffer() = default;
    ~CoalescingBuffer() = default;

    // Non-copyable
    CoalescingBuffer(const CoalescingBuffer&) = delete;
    CoalescingBuffer& operator=(const CoalescingBuffer&) = delete;

    /**
     * Push a market data update into the buffer.
     * If an update for this symbol already exists, it is replaced.
     * Thread-safe.
     */
    void push(const MarketData& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_[data.symbol] = data;
            hasUpdate_.store(true, std::memory_order_release);
        }
        cv_.notify_one();
    }

    /**
     * Drain all pending updates and return the list of affected symbols.
     * The buffer is cleared after draining.
     * Thread-safe.
     */
    std::vector<std::string> drainAffectedSymbols() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> symbols;
        symbols.reserve(pending_.size());

        for (const auto& [symbol, _] : pending_) {
            symbols.push_back(symbol);
        }

        pending_.clear();
        hasUpdate_.store(false, std::memory_order_release);

        return symbols;
    }

    /**
     * Drain all pending updates and return the market data for each.
     * The buffer is cleared after draining.
     * Thread-safe.
     */
    std::vector<MarketData> drainAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MarketData> updates;
        updates.reserve(pending_.size());

        for (const auto& [_, data] : pending_) {
            updates.push_back(data);
        }

        pending_.clear();
        hasUpdate_.store(false, std::memory_order_release);

        return updates;
    }

    /**
     * Check if there are any pending updates.
     * Thread-safe (lock-free read).
     */
    bool hasUpdates() const {
        return hasUpdate_.load(std::memory_order_acquire);
    }

    /**
     * Wait until at least one update arrives or timeout.
     * Returns true if updates available, false on timeout.
     * Thread-safe.
     */
    bool waitForUpdates(int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            return !pending_.empty();
        });
    }

    /**
     * Get the number of pending updates.
     * Thread-safe.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

    /**
     * Clear all pending updates.
     * Thread-safe.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        hasUpdate_.store(false, std::memory_order_release);
    }

private:
    std::unordered_map<std::string, MarketData> pending_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> hasUpdate_{false};
};

} // namespace TriArb
