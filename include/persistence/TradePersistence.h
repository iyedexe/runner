#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <optional>

/**
 * Trade status for persistence
 */
enum class TradeStatus {
    EXECUTED,   // Trade executed successfully
    PARTIAL,    // Partial fill
    FAILED,     // Trade failed
    ROLLBACK    // Rollback/recovery trade
};

/**
 * Trade type indicating leg position in arbitrage sequence
 */
enum class TradeType {
    ENTRY,          // First leg (leg 1)
    INTERMEDIATE,   // Middle leg (leg 2)
    EXIT            // Final leg (leg 3)
};

/**
 * Trade record for CSV persistence
 */
struct TradeRecord {
    std::string tradeId;            // Unique trade ID (clOrdId)
    std::string parentTradeId;      // ID of first trade in sequence
    TradeType tradeType;            // ENTRY, INTERMEDIATE, EXIT
    std::string symbol;             // Trading pair
    std::string side;               // BUY or SELL
    double intendedPrice;           // Estimated price before execution
    double intendedQty;             // Intended quantity
    double actualPrice;             // Actual fill price
    double actualQty;               // Actual filled quantity
    TradeStatus status;             // EXECUTED, PARTIAL, FAILED, ROLLBACK
    double pnl;                     // PnL amount (only for EXIT trades)
    double pnlPct;                  // PnL percentage (only for EXIT trades)
    std::chrono::system_clock::time_point timestamp;
};

/**
 * TradePersistence - Thread-safe CSV trade logger for auditing and analysis.
 *
 * Features:
 * - Daily rotating CSV files (trades_YYYYMMDD.csv)
 * - Thread-safe writing via mutex
 * - Automatic header creation for new files
 * - Configurable output directory
 *
 * Usage:
 *   TradePersistence persistence("/path/to/trades");
 *   persistence.startArbitrageSequence();  // Get parent_trade_id
 *   persistence.recordTrade(record);       // Record each leg
 */
class TradePersistence {
public:
    /**
     * Construct TradePersistence with output directory.
     * @param outputDir Directory for CSV files. Created if doesn't exist.
     */
    explicit TradePersistence(const std::string& outputDir);

    /**
     * Destructor - flushes and closes any open file.
     */
    ~TradePersistence();

    // Non-copyable, non-movable (owns file handle and mutex)
    TradePersistence(const TradePersistence&) = delete;
    TradePersistence& operator=(const TradePersistence&) = delete;
    TradePersistence(TradePersistence&&) = delete;
    TradePersistence& operator=(TradePersistence&&) = delete;

    /**
     * Start a new arbitrage sequence and return the parent trade ID.
     * This ID links all legs of the arbitrage together.
     * @return Unique parent trade ID for the sequence
     */
    [[nodiscard]] std::string startArbitrageSequence();

    /**
     * Record a single trade to the CSV file.
     * Thread-safe.
     * @param record Trade record to persist
     * @return true on success, false on write failure
     */
    bool recordTrade(const TradeRecord& record);

    /**
     * Record a trade with individual parameters (convenience method).
     * Thread-safe.
     */
    bool recordTrade(
        const std::string& tradeId,
        const std::string& parentTradeId,
        TradeType tradeType,
        const std::string& symbol,
        const std::string& side,
        double intendedPrice,
        double intendedQty,
        double actualPrice,
        double actualQty,
        TradeStatus status,
        double pnl = 0.0,
        double pnlPct = 0.0,
        std::optional<std::chrono::system_clock::time_point> timestamp = std::nullopt
    );

    /**
     * Flush pending writes to disk.
     * Thread-safe.
     */
    void flush();

    /**
     * Get the current output directory.
     */
    [[nodiscard]] const std::string& outputDir() const { return outputDir_; }

private:
    /**
     * Get current date string (YYYYMMDD format).
     */
    [[nodiscard]] static std::string getCurrentDateString();

    /**
     * Generate filename for the current date.
     */
    [[nodiscard]] std::string getFilename() const;

    /**
     * Ensure the output file is open and has headers.
     * Must be called with mutex held.
     * @return true if file is ready for writing
     */
    bool ensureFileReady();

    /**
     * Write CSV header row.
     * Must be called with mutex held.
     */
    void writeHeader();

    /**
     * Format TradeStatus as string.
     */
    [[nodiscard]] static std::string statusToString(TradeStatus status);

    /**
     * Format TradeType as string.
     */
    [[nodiscard]] static std::string tradeTypeToString(TradeType type);

    /**
     * Format timestamp as ISO 8601 string.
     */
    [[nodiscard]] static std::string formatTimestamp(
        const std::chrono::system_clock::time_point& tp);

    /**
     * Escape a string for CSV (handle quotes and commas).
     */
    [[nodiscard]] static std::string escapeCSV(const std::string& value);

    /**
     * Generate a unique sequence ID.
     */
    [[nodiscard]] std::string generateSequenceId();

    std::string outputDir_;
    std::string currentDate_;
    std::ofstream file_;
    mutable std::mutex mutex_;
    uint64_t sequenceCounter_{0};
};
