#pragma once

#include <cmath>
#include <string>
#include <optional>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace Filters {

/**
 * PRICE_FILTER - Defines price rules for a symbol
 * - minPrice: minimum price allowed (disabled if 0)
 * - maxPrice: maximum price allowed (disabled if 0)
 * - tickSize: price increment intervals (disabled if 0)
 */
struct PriceFilter {
    double minPrice = 0;
    double maxPrice = 0;
    double tickSize = 0;
    int precision = 0;

    bool isValid() const { return tickSize > 0 || minPrice > 0 || maxPrice > 0; }

    double roundPrice(double price) const {
        if (tickSize <= 0) return price;
        double rounded = std::floor(price / tickSize) * tickSize;
        if (minPrice > 0) rounded = std::max(minPrice, rounded);
        if (maxPrice > 0) rounded = std::min(maxPrice, rounded);
        return rounded;
    }

    bool validatePrice(double price) const {
        if (minPrice > 0 && price < minPrice) return false;
        if (maxPrice > 0 && price > maxPrice) return false;
        if (tickSize > 0 && std::fmod(price, tickSize) > 1e-10) return false;
        return true;
    }
};

/**
 * LOT_SIZE - Defines quantity rules for a symbol
 * - minQty: minimum quantity allowed
 * - maxQty: maximum quantity allowed
 * - stepSize: quantity increment intervals
 */
struct LotSizeFilter {
    double minQty = 0;
    double maxQty = 0;
    double stepSize = 0;
    int precision = 0;

    bool isValid() const { return stepSize > 0 || minQty > 0 || maxQty > 0; }

    double roundQty(double qty) const {
        if (stepSize <= 0) return qty;
        double rounded = std::floor(qty / stepSize) * stepSize;
        if (minQty > 0) rounded = std::max(minQty, rounded);
        if (maxQty > 0) rounded = std::min(maxQty, rounded);
        return rounded;
    }

    bool validateQty(double qty) const {
        if (minQty > 0 && qty < minQty) return false;
        if (maxQty > 0 && qty > maxQty) return false;
        if (stepSize > 0 && std::fmod(qty, stepSize) > 1e-10) return false;
        return true;
    }
};

/**
 * MARKET_LOT_SIZE - Defines quantity rules for MARKET orders
 */
struct MarketLotSizeFilter {
    double minQty = 0;
    double maxQty = 0;
    double stepSize = 0;
    int precision = 0;

    bool isValid() const { return stepSize > 0 || minQty > 0 || maxQty > 0; }

    double roundQty(double qty) const {
        if (stepSize <= 0) return qty;
        double rounded = std::floor(qty / stepSize) * stepSize;
        if (minQty > 0) rounded = std::max(minQty, rounded);
        if (maxQty > 0) rounded = std::min(maxQty, rounded);
        return rounded;
    }

    bool validateQty(double qty) const {
        if (minQty > 0 && qty < minQty) return false;
        if (maxQty > 0 && qty > maxQty) return false;
        if (stepSize > 0 && std::fmod(qty, stepSize) > 1e-10) return false;
        return true;
    }
};

/**
 * MIN_NOTIONAL - Defines minimum notional value (price * quantity)
 */
struct MinNotionalFilter {
    double minNotional = 0;
    bool applyToMarket = true;
    int avgPriceMins = 5;

    bool isValid() const { return minNotional > 0; }

    bool validateNotional(double price, double qty, bool isMarketOrder = false) const {
        if (!isValid()) return true;
        if (isMarketOrder && !applyToMarket) return true;
        return (price * qty) >= minNotional;
    }

    double minQtyForPrice(double price) const {
        if (price <= 0 || minNotional <= 0) return 0;
        return minNotional / price;
    }
};

/**
 * NOTIONAL - Defines acceptable notional range (price * quantity)
 */
struct NotionalFilter {
    double minNotional = 0;
    double maxNotional = 0;
    bool applyMinToMarket = false;
    bool applyMaxToMarket = false;
    int avgPriceMins = 5;

    bool isValid() const { return minNotional > 0 || maxNotional > 0; }

    bool validateNotional(double price, double qty, bool isMarketOrder = false) const {
        double notional = price * qty;
        if (minNotional > 0) {
            if (!isMarketOrder || applyMinToMarket) {
                if (notional < minNotional) return false;
            }
        }
        if (maxNotional > 0) {
            if (!isMarketOrder || applyMaxToMarket) {
                if (notional > maxNotional) return false;
            }
        }
        return true;
    }
};

/**
 * PERCENT_PRICE - Defines valid price range based on weighted average price
 */
struct PercentPriceFilter {
    double multiplierUp = 0;
    double multiplierDown = 0;
    int avgPriceMins = 5;

    bool isValid() const { return multiplierUp > 0 && multiplierDown > 0; }

    bool validatePrice(double price, double weightedAvgPrice) const {
        if (!isValid() || weightedAvgPrice <= 0) return true;
        double maxPrice = weightedAvgPrice * multiplierUp;
        double minPrice = weightedAvgPrice * multiplierDown;
        return price >= minPrice && price <= maxPrice;
    }
};

/**
 * PERCENT_PRICE_BY_SIDE - Defines valid price range based on side
 */
struct PercentPriceBySideFilter {
    double bidMultiplierUp = 0;
    double bidMultiplierDown = 0;
    double askMultiplierUp = 0;
    double askMultiplierDown = 0;
    int avgPriceMins = 1;

    bool isValid() const {
        return bidMultiplierUp > 0 && bidMultiplierDown > 0 &&
               askMultiplierUp > 0 && askMultiplierDown > 0;
    }

    bool validateBuyPrice(double price, double weightedAvgPrice) const {
        if (!isValid() || weightedAvgPrice <= 0) return true;
        double maxPrice = weightedAvgPrice * bidMultiplierUp;
        double minPrice = weightedAvgPrice * bidMultiplierDown;
        return price >= minPrice && price <= maxPrice;
    }

    bool validateSellPrice(double price, double weightedAvgPrice) const {
        if (!isValid() || weightedAvgPrice <= 0) return true;
        double maxPrice = weightedAvgPrice * askMultiplierUp;
        double minPrice = weightedAvgPrice * askMultiplierDown;
        return price >= minPrice && price <= maxPrice;
    }
};

/**
 * ICEBERG_PARTS - Defines maximum parts an iceberg order can have
 */
struct IcebergPartsFilter {
    int limit = 0;

    bool isValid() const { return limit > 0; }

    bool validateIceberg(double qty, double icebergQty) const {
        if (!isValid() || icebergQty <= 0) return true;
        int parts = static_cast<int>(std::ceil(qty / icebergQty));
        return parts <= limit;
    }
};

/**
 * MAX_NUM_ORDERS - Maximum number of orders allowed on a symbol
 */
struct MaxNumOrdersFilter {
    int maxNumOrders = 0;

    bool isValid() const { return maxNumOrders > 0; }
};

/**
 * MAX_NUM_ALGO_ORDERS - Maximum number of algo orders
 */
struct MaxNumAlgoOrdersFilter {
    int maxNumAlgoOrders = 0;

    bool isValid() const { return maxNumAlgoOrders > 0; }
};

/**
 * MAX_NUM_ICEBERG_ORDERS - Maximum number of iceberg orders
 */
struct MaxNumIcebergOrdersFilter {
    int maxNumIcebergOrders = 0;

    bool isValid() const { return maxNumIcebergOrders > 0; }
};

/**
 * MAX_POSITION - Maximum position allowed on base asset
 */
struct MaxPositionFilter {
    double maxPosition = 0;

    bool isValid() const { return maxPosition > 0; }

    bool validatePosition(double currentPosition, double orderQty) const {
        if (!isValid()) return true;
        return (currentPosition + orderQty) <= maxPosition;
    }
};

/**
 * TRAILING_DELTA - Defines min/max trailing delta values
 */
struct TrailingDeltaFilter {
    long minTrailingAboveDelta = 0;
    long maxTrailingAboveDelta = 0;
    long minTrailingBelowDelta = 0;
    long maxTrailingBelowDelta = 0;

    bool isValid() const {
        return maxTrailingAboveDelta > 0 || maxTrailingBelowDelta > 0;
    }

    bool validateAboveDelta(long delta) const {
        if (minTrailingAboveDelta > 0 && delta < minTrailingAboveDelta) return false;
        if (maxTrailingAboveDelta > 0 && delta > maxTrailingAboveDelta) return false;
        return true;
    }

    bool validateBelowDelta(long delta) const {
        if (minTrailingBelowDelta > 0 && delta < minTrailingBelowDelta) return false;
        if (maxTrailingBelowDelta > 0 && delta > maxTrailingBelowDelta) return false;
        return true;
    }
};

/**
 * MAX_NUM_ORDER_AMENDS - Maximum number of order amendments
 */
struct MaxNumOrderAmendsFilter {
    int maxNumOrderAmends = 0;

    bool isValid() const { return maxNumOrderAmends > 0; }
};

/**
 * MAX_NUM_ORDER_LISTS - Maximum number of open order lists
 */
struct MaxNumOrderListsFilter {
    int maxNumOrderLists = 0;

    bool isValid() const { return maxNumOrderLists > 0; }
};

} // namespace Filters

/**
 * SymbolFilters - Container for all filters on a symbol
 */
class SymbolFilters {
public:
    SymbolFilters() = default;

    // Parse filters from exchange info JSON
    static SymbolFilters fromJson(const nlohmann::json& filtersJson);

    // Accessors
    const Filters::PriceFilter& priceFilter() const { return priceFilter_; }
    const Filters::LotSizeFilter& lotSize() const { return lotSize_; }
    const Filters::MarketLotSizeFilter& marketLotSize() const { return marketLotSize_; }
    const Filters::MinNotionalFilter& minNotional() const { return minNotional_; }
    const Filters::NotionalFilter& notional() const { return notional_; }
    const Filters::PercentPriceFilter& percentPrice() const { return percentPrice_; }
    const Filters::PercentPriceBySideFilter& percentPriceBySide() const { return percentPriceBySide_; }
    const Filters::IcebergPartsFilter& icebergParts() const { return icebergParts_; }
    const Filters::MaxNumOrdersFilter& maxNumOrders() const { return maxNumOrders_; }
    const Filters::MaxNumAlgoOrdersFilter& maxNumAlgoOrders() const { return maxNumAlgoOrders_; }
    const Filters::MaxNumIcebergOrdersFilter& maxNumIcebergOrders() const { return maxNumIcebergOrders_; }
    const Filters::MaxPositionFilter& maxPosition() const { return maxPosition_; }
    const Filters::TrailingDeltaFilter& trailingDelta() const { return trailingDelta_; }
    const Filters::MaxNumOrderAmendsFilter& maxNumOrderAmends() const { return maxNumOrderAmends_; }
    const Filters::MaxNumOrderListsFilter& maxNumOrderLists() const { return maxNumOrderLists_; }

    // Convenience methods for order sizing
    double roundPrice(double price) const { return priceFilter_.roundPrice(price); }
    double roundQty(double qty) const { return lotSize_.roundQty(qty); }
    double roundMarketQty(double qty) const {
        return marketLotSize_.isValid() ? marketLotSize_.roundQty(qty) : lotSize_.roundQty(qty);
    }

    // Price precision (number of decimal places)
    int pricePrecision() const { return priceFilter_.precision; }
    // Quantity precision
    int qtyPrecision() const { return lotSize_.precision; }

    // Validation
    bool validatePrice(double price) const { return priceFilter_.validatePrice(price); }
    bool validateQty(double qty) const { return lotSize_.validateQty(qty); }
    bool validateMarketQty(double qty) const {
        return marketLotSize_.isValid() ? marketLotSize_.validateQty(qty) : lotSize_.validateQty(qty);
    }
    bool validateNotional(double price, double qty, bool isMarketOrder = false) const {
        if (notional_.isValid()) {
            return notional_.validateNotional(price, qty, isMarketOrder);
        }
        return minNotional_.validateNotional(price, qty, isMarketOrder);
    }

    // Get minimum quantity to meet notional requirement at given price
    double minQtyForNotional(double price) const {
        double minQty = lotSize_.minQty;
        if (minNotional_.isValid()) {
            double notionalMinQty = minNotional_.minQtyForPrice(price);
            minQty = std::max(minQty, notionalMinQty);
        }
        if (notional_.minNotional > 0) {
            double notionalMinQty = notional_.minNotional / price;
            minQty = std::max(minQty, notionalMinQty);
        }
        return lotSize_.roundQty(minQty + lotSize_.stepSize); // Round up
    }

private:
    Filters::PriceFilter priceFilter_;
    Filters::LotSizeFilter lotSize_;
    Filters::MarketLotSizeFilter marketLotSize_;
    Filters::MinNotionalFilter minNotional_;
    Filters::NotionalFilter notional_;
    Filters::PercentPriceFilter percentPrice_;
    Filters::PercentPriceBySideFilter percentPriceBySide_;
    Filters::IcebergPartsFilter icebergParts_;
    Filters::MaxNumOrdersFilter maxNumOrders_;
    Filters::MaxNumAlgoOrdersFilter maxNumAlgoOrders_;
    Filters::MaxNumIcebergOrdersFilter maxNumIcebergOrders_;
    Filters::MaxPositionFilter maxPosition_;
    Filters::TrailingDeltaFilter trailingDelta_;
    Filters::MaxNumOrderAmendsFilter maxNumOrderAmends_;
    Filters::MaxNumOrderListsFilter maxNumOrderLists_;
};
