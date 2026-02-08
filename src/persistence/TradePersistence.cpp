#include "persistence/TradePersistence.h"
#include "logger.hpp"

#include <iomanip>
#include <sstream>
#include <ctime>

TradePersistence::TradePersistence(const std::string& outputDir)
    : outputDir_(outputDir)
{
    // Create output directory if it doesn't exist
    std::error_code ec;
    std::filesystem::create_directories(outputDir_, ec);
    if (ec) {
        LOG_ERROR("[TradePersistence] Failed to create directory {}: {}",
                  outputDir_, ec.message());
    } else {
        LOG_INFO("[TradePersistence] Initialized with output directory: {}", outputDir_);
    }
}

TradePersistence::~TradePersistence() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

std::string TradePersistence::startArbitrageSequence() {
    std::lock_guard<std::mutex> lock(mutex_);
    return generateSequenceId();
}

std::string TradePersistence::generateSequenceId() {
    // Generate unique ID: timestamp_counter
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();

    std::ostringstream oss;
    oss << "ARB_" << millis << "_" << (++sequenceCounter_);
    return oss.str();
}

std::string TradePersistence::getCurrentDateString() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};

#ifdef _WIN32
    gmtime_s(&tm_buf, &time);
#else
    gmtime_r(&time, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d");
    return oss.str();
}

std::string TradePersistence::getFilename() const {
    return outputDir_ + "/trades_" + getCurrentDateString() + ".csv";
}

bool TradePersistence::ensureFileReady() {
    std::string currentDate = getCurrentDateString();

    // Check if we need to rotate to a new file (new day)
    if (currentDate != currentDate_ || !file_.is_open()) {
        // Close existing file
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }

        currentDate_ = currentDate;
        std::string filename = getFilename();

        // Check if file exists (to determine if we need headers)
        bool fileExists = std::filesystem::exists(filename);

        // Open file in append mode
        file_.open(filename, std::ios::app);
        if (!file_.is_open()) {
            LOG_ERROR("[TradePersistence] Failed to open file: {}", filename);
            return false;
        }

        // Write header if this is a new file
        if (!fileExists) {
            writeHeader();
            LOG_INFO("[TradePersistence] Created new trade log: {}", filename);
        } else {
            LOG_INFO("[TradePersistence] Appending to existing trade log: {}", filename);
        }
    }

    return file_.is_open() && file_.good();
}

void TradePersistence::writeHeader() {
    file_ << "trade_id,"
          << "parent_trade_id,"
          << "trade_type,"
          << "symbol,"
          << "side,"
          << "intended_price,"
          << "intended_qty,"
          << "actual_price,"
          << "actual_qty,"
          << "status,"
          << "pnl,"
          << "pnl_pct,"
          << "timestamp"
          << "\n";
    file_.flush();
}

std::string TradePersistence::statusToString(TradeStatus status) {
    switch (status) {
        case TradeStatus::EXECUTED: return "EXECUTED";
        case TradeStatus::PARTIAL:  return "PARTIAL";
        case TradeStatus::FAILED:   return "FAILED";
        case TradeStatus::ROLLBACK: return "ROLLBACK";
        default:                    return "UNKNOWN";
    }
}

std::string TradePersistence::tradeTypeToString(TradeType type) {
    switch (type) {
        case TradeType::ENTRY:        return "ENTRY";
        case TradeType::INTERMEDIATE: return "INTERMEDIATE";
        case TradeType::EXIT:         return "EXIT";
        default:                      return "UNKNOWN";
    }
}

std::string TradePersistence::formatTimestamp(
    const std::chrono::system_clock::time_point& tp)
{
    auto time = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time);
#else
    gmtime_r(&time, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << 'Z';
    return oss.str();
}

std::string TradePersistence::escapeCSV(const std::string& value) {
    // If the value contains comma, quote, or newline, wrap in quotes
    bool needsQuotes = false;
    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes) {
        return value;
    }

    // Escape quotes by doubling them and wrap in quotes
    std::ostringstream oss;
    oss << '"';
    for (char c : value) {
        if (c == '"') {
            oss << "\"\"";
        } else {
            oss << c;
        }
    }
    oss << '"';
    return oss.str();
}

bool TradePersistence::recordTrade(const TradeRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!ensureFileReady()) {
        return false;
    }

    // Write CSV row with fixed precision for doubles
    file_ << escapeCSV(record.tradeId) << ','
          << escapeCSV(record.parentTradeId) << ','
          << tradeTypeToString(record.tradeType) << ','
          << escapeCSV(record.symbol) << ','
          << escapeCSV(record.side) << ','
          << std::fixed << std::setprecision(8) << record.intendedPrice << ','
          << std::fixed << std::setprecision(8) << record.intendedQty << ','
          << std::fixed << std::setprecision(8) << record.actualPrice << ','
          << std::fixed << std::setprecision(8) << record.actualQty << ','
          << statusToString(record.status) << ','
          << std::fixed << std::setprecision(8) << record.pnl << ','
          << std::fixed << std::setprecision(4) << record.pnlPct << ','
          << formatTimestamp(record.timestamp)
          << "\n";

    // Flush after each trade for durability
    file_.flush();

    if (!file_.good()) {
        LOG_ERROR("[TradePersistence] Write failed for trade: {}", record.tradeId);
        return false;
    }

    LOG_DEBUG("[TradePersistence] Recorded trade: {} ({})",
              record.tradeId, tradeTypeToString(record.tradeType));
    return true;
}

bool TradePersistence::recordTrade(
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
    double pnl,
    double pnlPct,
    std::optional<std::chrono::system_clock::time_point> timestamp)
{
    TradeRecord record{
        .tradeId = tradeId,
        .parentTradeId = parentTradeId,
        .tradeType = tradeType,
        .symbol = symbol,
        .side = side,
        .intendedPrice = intendedPrice,
        .intendedQty = intendedQty,
        .actualPrice = actualPrice,
        .actualQty = actualQty,
        .status = status,
        .pnl = pnl,
        .pnlPct = pnlPct,
        .timestamp = timestamp.value_or(std::chrono::system_clock::now())
    };

    return recordTrade(record);
}

void TradePersistence::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}
