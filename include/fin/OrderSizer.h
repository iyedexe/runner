#pragma once

#include <string>
#include <map>
#include <array>
#include <optional>
#include <stdexcept>
#include "fin/SymbolFilters.h"
#include "market_connection/OrderBook.h"  // For SymbolId, MAX_SYMBOLS, SymbolRegistry
#include "logger.hpp"

/**
 * OrderValidationResult - Result of order validation
 */
struct OrderValidationResult {
    bool valid = false;
    std::string reason;

    operator bool() const { return valid; }

    static OrderValidationResult success() {
        return {true, ""};
    }

    static OrderValidationResult failure(const std::string& reason) {
        return {false, reason};
    }
};

/**
 * AdjustedOrder - Order parameters adjusted to meet filter requirements
 */
struct AdjustedOrder {
    double price = 0;
    double quantity = 0;
    bool wasAdjusted = false;

    // Validation result after adjustment
    OrderValidationResult validation;
};

/**
 * OrderSizer - Validates and adjusts orders to meet exchange filter requirements
 *
 * Usage:
 *   OrderSizer sizer;
 *   sizer.addSymbol("BTCUSDT", SymbolFilters::fromJson(filtersJson));
 *
 *   // Adjust order
 *   auto adjusted = sizer.adjustOrder("BTCUSDT", price, qty, isMarketOrder);
 *   if (adjusted.validation) {
 *       // Use adjusted.price and adjusted.quantity
 *   }
 *
 *   // Or just validate
 *   auto result = sizer.validateOrder("BTCUSDT", price, qty, isMarketOrder);
 */
class OrderSizer {
public:
    OrderSizer() = default;

    /**
     * Add or update filters for a symbol
     */
    void addSymbol(const std::string& symbol, const SymbolFilters& filters) {
        filters_[symbol] = filters;

        // Also populate the SymbolId-indexed array for O(1) lookups
        SymbolId id = SymbolRegistry::instance().getId(symbol);
        if (id != INVALID_SYMBOL_ID) {
            filtersBySymbolId_[id] = &filters_[symbol];
            hasFiltersBySymbolId_[id] = true;
        }

        LOG_DEBUG("[OrderSizer] Added {}: lotStep={}, lotPrec={}, mktStep={}, mktPrec={}",
                 symbol,
                 filters.lotSize().stepSize, filters.lotSize().precision,
                 filters.marketLotSize().stepSize, filters.marketLotSize().precision);
    }

    /**
     * Check if symbol is registered
     */
    bool hasSymbol(const std::string& symbol) const {
        return filters_.find(symbol) != filters_.end();
    }

    /**
     * Get filters for a symbol
     */
    const SymbolFilters& getFilters(const std::string& symbol) const {
        auto it = filters_.find(symbol);
        if (it == filters_.end()) {
            throw std::runtime_error("OrderSizer: Unknown symbol " + symbol);
        }
        return it->second;
    }

    /**
     * Validate an order without adjustment
     */
    OrderValidationResult validateOrder(
        const std::string& symbol,
        double price,
        double quantity,
        bool isMarketOrder = false,
        double weightedAvgPrice = 0) const
    {
        if (!hasSymbol(symbol)) {
            return OrderValidationResult::failure("Unknown symbol: " + symbol);
        }

        const auto& filters = getFilters(symbol);

        // For market orders, price validation is skipped (no price)
        if (!isMarketOrder) {
            // Validate price against PRICE_FILTER
            if (!filters.validatePrice(price)) {
                return OrderValidationResult::failure(
                    "Price " + std::to_string(price) + " fails PRICE_FILTER");
            }

            // Validate price against PERCENT_PRICE if we have weighted avg price
            if (weightedAvgPrice > 0 && filters.percentPrice().isValid()) {
                if (!filters.percentPrice().validatePrice(price, weightedAvgPrice)) {
                    return OrderValidationResult::failure(
                        "Price " + std::to_string(price) + " fails PERCENT_PRICE filter");
                }
            }
        }

        // Validate quantity
        if (isMarketOrder) {
            if (!filters.validateMarketQty(quantity)) {
                return OrderValidationResult::failure(
                    "Quantity " + std::to_string(quantity) + " fails MARKET_LOT_SIZE");
            }
        } else {
            if (!filters.validateQty(quantity)) {
                return OrderValidationResult::failure(
                    "Quantity " + std::to_string(quantity) + " fails LOT_SIZE");
            }
        }

        // Validate notional (price * quantity)
        // For market orders, we need the weighted average price
        double effectivePrice = isMarketOrder && weightedAvgPrice > 0 ? weightedAvgPrice : price;
        if (!filters.validateNotional(effectivePrice, quantity, isMarketOrder)) {
            return OrderValidationResult::failure(
                "Notional " + std::to_string(effectivePrice * quantity) + " fails NOTIONAL filter");
        }

        return OrderValidationResult::success();
    }

    /**
     * Adjust an order to meet filter requirements
     */
    AdjustedOrder adjustOrder(
        const std::string& symbol,
        double price,
        double quantity,
        bool isMarketOrder = false,
        double weightedAvgPrice = 0) const
    {
        AdjustedOrder result;
        result.price = price;
        result.quantity = quantity;

        if (!hasSymbol(symbol)) {
            result.validation = OrderValidationResult::failure("Unknown symbol: " + symbol);
            return result;
        }

        const auto& filters = getFilters(symbol);

        // Adjust price (for limit orders)
        if (!isMarketOrder) {
            double adjustedPrice = filters.roundPrice(price);
            if (adjustedPrice != price) {
                result.price = adjustedPrice;
                result.wasAdjusted = true;
                LOG_DEBUG("[OrderSizer] Price adjusted: {} -> {}", price, adjustedPrice);
            }
        }

        // Adjust quantity
        double adjustedQty = isMarketOrder ? filters.roundMarketQty(quantity) : filters.roundQty(quantity);
        if (adjustedQty != quantity) {
            result.quantity = adjustedQty;
            result.wasAdjusted = true;
            LOG_DEBUG("[OrderSizer] Quantity adjusted: {} -> {}", quantity, adjustedQty);
        }

        // Check notional minimum and adjust quantity up if needed
        double effectivePrice = isMarketOrder && weightedAvgPrice > 0 ? weightedAvgPrice : result.price;
        if (effectivePrice > 0) {
            double minQty = filters.minQtyForNotional(effectivePrice);
            if (result.quantity < minQty) {
                double newQty = isMarketOrder ? filters.roundMarketQty(minQty) : filters.roundQty(minQty);
                if (newQty > result.quantity) {
                    LOG_DEBUG("[OrderSizer] Quantity increased for notional: {} -> {}", result.quantity, newQty);
                    result.quantity = newQty;
                    result.wasAdjusted = true;
                }
            }
        }

        // Final validation
        result.validation = validateOrder(symbol, result.price, result.quantity, isMarketOrder, weightedAvgPrice);

        return result;
    }

    /**
     * Get minimum quantity for a symbol at a given price
     */
    double getMinQuantity(const std::string& symbol, double price) const {
        if (!hasSymbol(symbol)) {
            throw std::runtime_error("OrderSizer: Unknown symbol " + symbol);
        }
        const auto& filters = getFilters(symbol);
        return filters.minQtyForNotional(price);
    }

    /**
     * Get maximum quantity for a symbol
     */
    double getMaxQuantity(const std::string& symbol, bool isMarketOrder = false) const {
        if (!hasSymbol(symbol)) {
            throw std::runtime_error("OrderSizer: Unknown symbol " + symbol);
        }
        const auto& filters = getFilters(symbol);
        if (isMarketOrder && filters.marketLotSize().isValid()) {
            return filters.marketLotSize().maxQty;
        }
        return filters.lotSize().maxQty;
    }

    /**
     * Round price to valid tick size
     */
    double roundPrice(const std::string& symbol, double price) const {
        if (!hasSymbol(symbol)) return price;
        return getFilters(symbol).roundPrice(price);
    }

    /**
     * Round quantity to valid step size
     */
    double roundQuantity(const std::string& symbol, double quantity, bool isMarketOrder = false) const {
        if (!hasSymbol(symbol)) {
            LOG_WARNING("[OrderSizer] Symbol {} not found, returning unrounded qty={:.10f}", symbol, quantity);
            return quantity;
        }
        const auto& filters = getFilters(symbol);
        double rounded = isMarketOrder ? filters.roundMarketQty(quantity) : filters.roundQty(quantity);
        if (rounded != quantity) {
            LOG_DEBUG("[OrderSizer] {} rounded: {:.10f} -> {:.10f} (mkt={})", symbol, quantity, rounded, isMarketOrder);
        }
        return rounded;
    }

    /**
     * Get price precision (number of decimal places)
     */
    int getPricePrecision(const std::string& symbol) const {
        if (!hasSymbol(symbol)) return 8;
        return getFilters(symbol).pricePrecision();
    }

    /**
     * Get quantity precision (number of decimal places)
     */
    int getQuantityPrecision(const std::string& symbol) const {
        if (!hasSymbol(symbol)) return 8;
        return getFilters(symbol).qtyPrecision();
    }

    /**
     * Get number of registered symbols
     */
    size_t symbolCount() const { return filters_.size(); }

    /**
     * Clear all symbols
     */
    void clear() {
        filters_.clear();
        filtersBySymbolId_.fill(nullptr);
        hasFiltersBySymbolId_.fill(false);
    }

    // Fast-path methods using SymbolId for O(1) lookups

    /**
     * Check if symbol is registered (by SymbolId)
     */
    [[nodiscard]] bool hasSymbol(SymbolId id) const noexcept {
        return hasFiltersBySymbolId_[id];
    }

    /**
     * Round quantity using SymbolId for O(1) lookup
     */
    [[nodiscard]] double roundQuantity(SymbolId id, double quantity, bool isMarketOrder = false) const {
        const auto* filters = filtersBySymbolId_[id];
        if (!filters) {
            return quantity;
        }
        return isMarketOrder ? filters->roundMarketQty(quantity) : filters->roundQty(quantity);
    }

    /**
     * Get filters for a symbol (by SymbolId)
     */
    [[nodiscard]] const SymbolFilters* getFilters(SymbolId id) const noexcept {
        return filtersBySymbolId_[id];
    }

private:
    std::map<std::string, SymbolFilters> filters_;

    // Parallel arrays indexed by SymbolId for O(1) lookups
    std::array<const SymbolFilters*, MAX_SYMBOLS> filtersBySymbolId_{};
    std::array<bool, MAX_SYMBOLS> hasFiltersBySymbolId_{};
};
