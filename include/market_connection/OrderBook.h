#pragma once

#include <atomic>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef __x86_64__
#include <immintrin.h>
#endif

// Symbol ID type - compact integer for O(1) array lookups
using SymbolId = uint16_t;
constexpr SymbolId INVALID_SYMBOL_ID = UINT16_MAX;
constexpr size_t MAX_SYMBOLS = 4096;

struct BidAsk {
    double bid = 0.0;
    double ask = 0.0;
};

/**
 * SymbolRegistry - Maps symbol strings to dense integer IDs for O(1) lookups.
 *
 * Thread safety: Registration is NOT thread-safe (done at initialization).
 * Lookups are thread-safe and lock-free after initialization.
 */
class SymbolRegistry {
public:
    static SymbolRegistry& instance() {
        static SymbolRegistry registry;
        return registry;
    }

    SymbolRegistry(const SymbolRegistry&) = delete;
    SymbolRegistry& operator=(const SymbolRegistry&) = delete;

    SymbolId registerSymbol(const std::string& symbol) {
        auto it = symbolToId_.find(symbol);
        if (it != symbolToId_.end()) {
            return it->second;
        }

        if (idToSymbol_.size() >= MAX_SYMBOLS) {
            throw std::runtime_error("SymbolRegistry: exceeded maximum symbols");
        }

        SymbolId id = static_cast<SymbolId>(idToSymbol_.size());
        symbolToId_[symbol] = id;
        idToSymbol_.push_back(symbol);
        return id;
    }

    std::vector<SymbolId> registerSymbols(const std::vector<std::string>& symbols) {
        std::vector<SymbolId> ids;
        ids.reserve(symbols.size());
        for (const auto& symbol : symbols) {
            ids.push_back(registerSymbol(symbol));
        }
        return ids;
    }

    [[nodiscard]] const std::string& getSymbol(SymbolId id) const noexcept {
        return idToSymbol_[id];
    }

    [[nodiscard]] SymbolId getId(const std::string& symbol) const {
        auto it = symbolToId_.find(symbol);
        return (it != symbolToId_.end()) ? it->second : INVALID_SYMBOL_ID;
    }

    [[nodiscard]] bool hasSymbol(const std::string& symbol) const {
        return symbolToId_.find(symbol) != symbolToId_.end();
    }

    [[nodiscard]] size_t size() const noexcept {
        return idToSymbol_.size();
    }

    void clear() {
        symbolToId_.clear();
        idToSymbol_.clear();
    }

private:
    SymbolRegistry() {
        idToSymbol_.reserve(MAX_SYMBOLS);
    }

    std::unordered_map<std::string, SymbolId> symbolToId_;
    std::vector<std::string> idToSymbol_;
};

/**
 * Cache-line aligned atomic price slot with sequence lock.
 */
struct alignas(64) AtomicPriceSlot {
    std::atomic<uint64_t> sequence{0};
    double bid{0.0};
    double ask{0.0};
    char padding_[64 - sizeof(std::atomic<uint64_t>) - 2 * sizeof(double)];

    AtomicPriceSlot() = default;
    AtomicPriceSlot(const AtomicPriceSlot&) = delete;
    AtomicPriceSlot& operator=(const AtomicPriceSlot&) = delete;
};
static_assert(sizeof(AtomicPriceSlot) == 64, "AtomicPriceSlot must be cache-line sized");

/**
 * OrderBook - High-performance price storage using SeqLock pattern.
 *
 * Design:
 * - Uses SymbolId (integer) for O(1) array indexing
 * - SeqLock: writers increment sequence before/after update
 * - Readers retry if sequence changed (torn read detection)
 * - No locks on read path
 *
 * Performance:
 * - Write: ~10-30ns
 * - Read: ~5-20ns (wait-free)
 */
class OrderBook {
public:
    OrderBook() {
        updatedBits_.reset();
    }

    /**
     * Update by symbol ID (hot path - preferred).
     * Only updates non-zero values to handle partial updates (bid-only or ask-only).
     */
    void update(SymbolId id, double bid, double ask) noexcept {
        // Skip if both values are zero (no actual update)
        if (bid == 0.0 && ask == 0.0) {
            return;
        }

        auto& slot = data_[id];

        uint64_t seq = slot.sequence.load(std::memory_order_relaxed);
        slot.sequence.store(seq + 1, std::memory_order_release);

        std::atomic_thread_fence(std::memory_order_release);

        // Only update non-zero values
        if (bid > 0.0) {
            slot.bid = bid;
        }
        if (ask > 0.0) {
            slot.ask = ask;
        }

        std::atomic_thread_fence(std::memory_order_release);
        slot.sequence.store(seq + 2, std::memory_order_release);

        // Set atomic flag BEFORE acquiring mutex for lock-free fast-path
        hasUpdatesAtomic_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(updateMtx_);
            updatedBits_.set(id);
            hasUpdates_ = true;
        }
        updateCv_.notify_one();
    }

    /**
     * Update by symbol string (convenience - registers symbol if needed).
     */
    void update(const std::string& symbol, double bid, double ask) {
        SymbolId id = SymbolRegistry::instance().registerSymbol(symbol);
        update(id, bid, ask);
    }

    /**
     * Get price by symbol ID (hot path - wait-free).
     */
    [[nodiscard]] BidAsk get(SymbolId id) const noexcept {
        const auto& slot = data_[id];
        BidAsk result;
        uint64_t seq1, seq2;

        do {
            seq1 = slot.sequence.load(std::memory_order_acquire);

            if (seq1 & 1) {
#ifdef __x86_64__
                _mm_pause();
#endif
                continue;
            }

            result.bid = slot.bid;
            result.ask = slot.ask;

            std::atomic_thread_fence(std::memory_order_acquire);
            seq2 = slot.sequence.load(std::memory_order_acquire);
        } while (seq1 != seq2);

        return result;
    }

    /**
     * Get price by symbol string (convenience).
     */
    [[nodiscard]] BidAsk get(const std::string& symbol) const {
        SymbolId id = SymbolRegistry::instance().getId(symbol);
        if (id == INVALID_SYMBOL_ID) {
            return {0.0, 0.0};
        }
        return get(id);
    }

    /**
     * Batch read 3 symbols (optimized for triangular arbitrage).
     */
    void getTriple(SymbolId id0, SymbolId id1, SymbolId id2,
                   BidAsk& out0, BidAsk& out1, BidAsk& out2) const noexcept {
#ifdef __x86_64__
        _mm_prefetch(reinterpret_cast<const char*>(&data_[id0]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&data_[id1]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&data_[id2]), _MM_HINT_T0);
#endif
        out0 = get(id0);
        out1 = get(id1);
        out2 = get(id2);
    }

    /**
     * Wait for updates, returns bitmap of updated symbols.
     */
    std::bitset<MAX_SYMBOLS> waitForUpdates() {
        std::unique_lock<std::mutex> lock(updateMtx_);
        updateCv_.wait(lock, [this] { return hasUpdates_; });

        std::bitset<MAX_SYMBOLS> result = updatedBits_;
        updatedBits_.reset();
        hasUpdates_ = false;
        hasUpdatesAtomic_.store(false, std::memory_order_release);
        return result;
    }

    /**
     * Wait for updates with timeout for periodic shutdown checks.
     * Returns empty bitset on timeout.
     */
    std::bitset<MAX_SYMBOLS> waitForUpdatesWithTimeout(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(updateMtx_);
        bool gotUpdate = updateCv_.wait_for(lock, timeout, [this] { return hasUpdates_; });

        if (!gotUpdate) {
            return std::bitset<MAX_SYMBOLS>();  // Timeout - return empty
        }

        std::bitset<MAX_SYMBOLS> result = updatedBits_;
        updatedBits_.reset();
        hasUpdates_ = false;
        hasUpdatesAtomic_.store(false, std::memory_order_release);
        return result;
    }

    /**
     * Busy-poll for updates with spin limit.
     * Uses lock-free atomic check to avoid mutex acquisition on every iteration.
     */
    std::bitset<MAX_SYMBOLS> waitForUpdatesSpin(int maxSpins = 10000) {
        for (int i = 0; i < maxSpins; ++i) {
            // Fast-path: check atomic without lock
            if (hasUpdatesAtomic_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(updateMtx_);
                if (hasUpdates_) {
                    std::bitset<MAX_SYMBOLS> result = updatedBits_;
                    updatedBits_.reset();
                    hasUpdates_ = false;
                    hasUpdatesAtomic_.store(false, std::memory_order_release);
                    return result;
                }
            }
#ifdef __x86_64__
            _mm_pause();
#endif
        }
        return waitForUpdates();
    }

    /**
     * Non-blocking check for updates.
     */
    std::bitset<MAX_SYMBOLS> consumeUpdates() {
        std::lock_guard<std::mutex> lock(updateMtx_);
        if (!hasUpdates_) {
            return std::bitset<MAX_SYMBOLS>();
        }

        std::bitset<MAX_SYMBOLS> result = updatedBits_;
        updatedBits_.reset();
        hasUpdates_ = false;
        hasUpdatesAtomic_.store(false, std::memory_order_release);
        return result;
    }

    [[nodiscard]] bool hasUpdates() const {
        std::lock_guard<std::mutex> lock(updateMtx_);
        return hasUpdates_;
    }

    [[nodiscard]] size_t size() const {
        return SymbolRegistry::instance().size();
    }

private:
    std::array<AtomicPriceSlot, MAX_SYMBOLS> data_{};

    mutable std::mutex updateMtx_;
    std::condition_variable updateCv_;
    std::bitset<MAX_SYMBOLS> updatedBits_;
    bool hasUpdates_ = false;
    std::atomic<bool> hasUpdatesAtomic_{false};  // Lock-free fast-path check
};
