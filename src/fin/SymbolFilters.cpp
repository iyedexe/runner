#include "fin/SymbolFilters.h"
#include <cmath>

namespace {
    int computePrecision(double stepOrTick) {
        if (stepOrTick <= 0) return 0;
        int precision = 0;
        while (stepOrTick < 1.0 && precision < 10) {
            stepOrTick *= 10;
            precision++;
        }
        return precision;
    }

    double safeStod(const nlohmann::json& j, const std::string& key, double defaultVal = 0) {
        if (!j.contains(key)) return defaultVal;
        if (j[key].is_string()) {
            return std::stod(j[key].get<std::string>());
        } else if (j[key].is_number()) {
            return j[key].get<double>();
        }
        return defaultVal;
    }

    int safeStoi(const nlohmann::json& j, const std::string& key, int defaultVal = 0) {
        if (!j.contains(key)) return defaultVal;
        if (j[key].is_number()) {
            return j[key].get<int>();
        }
        return defaultVal;
    }

    long safeStol(const nlohmann::json& j, const std::string& key, long defaultVal = 0) {
        if (!j.contains(key)) return defaultVal;
        if (j[key].is_number()) {
            return j[key].get<long>();
        }
        return defaultVal;
    }

    bool safeBool(const nlohmann::json& j, const std::string& key, bool defaultVal = false) {
        if (!j.contains(key)) return defaultVal;
        if (j[key].is_boolean()) {
            return j[key].get<bool>();
        }
        return defaultVal;
    }
}

SymbolFilters SymbolFilters::fromJson(const nlohmann::json& filtersJson) {
    SymbolFilters filters;

    for (const auto& filter : filtersJson) {
        if (!filter.contains("filterType")) continue;

        std::string filterType = filter["filterType"].get<std::string>();

        if (filterType == "PRICE_FILTER") {
            filters.priceFilter_.minPrice = safeStod(filter, "minPrice");
            filters.priceFilter_.maxPrice = safeStod(filter, "maxPrice");
            filters.priceFilter_.tickSize = safeStod(filter, "tickSize");
            filters.priceFilter_.precision = computePrecision(filters.priceFilter_.tickSize);
        }
        else if (filterType == "LOT_SIZE") {
            filters.lotSize_.minQty = safeStod(filter, "minQty");
            filters.lotSize_.maxQty = safeStod(filter, "maxQty");
            filters.lotSize_.stepSize = safeStod(filter, "stepSize");
            filters.lotSize_.precision = computePrecision(filters.lotSize_.stepSize);
        }
        else if (filterType == "MARKET_LOT_SIZE") {
            filters.marketLotSize_.minQty = safeStod(filter, "minQty");
            filters.marketLotSize_.maxQty = safeStod(filter, "maxQty");
            filters.marketLotSize_.stepSize = safeStod(filter, "stepSize");
            filters.marketLotSize_.precision = computePrecision(filters.marketLotSize_.stepSize);
        }
        else if (filterType == "MIN_NOTIONAL") {
            filters.minNotional_.minNotional = safeStod(filter, "minNotional");
            filters.minNotional_.applyToMarket = safeBool(filter, "applyToMarket", true);
            filters.minNotional_.avgPriceMins = safeStoi(filter, "avgPriceMins", 5);
        }
        else if (filterType == "NOTIONAL") {
            filters.notional_.minNotional = safeStod(filter, "minNotional");
            filters.notional_.maxNotional = safeStod(filter, "maxNotional");
            filters.notional_.applyMinToMarket = safeBool(filter, "applyMinToMarket");
            filters.notional_.applyMaxToMarket = safeBool(filter, "applyMaxToMarket");
            filters.notional_.avgPriceMins = safeStoi(filter, "avgPriceMins", 5);
        }
        else if (filterType == "PERCENT_PRICE") {
            filters.percentPrice_.multiplierUp = safeStod(filter, "multiplierUp");
            filters.percentPrice_.multiplierDown = safeStod(filter, "multiplierDown");
            filters.percentPrice_.avgPriceMins = safeStoi(filter, "avgPriceMins", 5);
        }
        else if (filterType == "PERCENT_PRICE_BY_SIDE") {
            filters.percentPriceBySide_.bidMultiplierUp = safeStod(filter, "bidMultiplierUp");
            filters.percentPriceBySide_.bidMultiplierDown = safeStod(filter, "bidMultiplierDown");
            filters.percentPriceBySide_.askMultiplierUp = safeStod(filter, "askMultiplierUp");
            filters.percentPriceBySide_.askMultiplierDown = safeStod(filter, "askMultiplierDown");
            filters.percentPriceBySide_.avgPriceMins = safeStoi(filter, "avgPriceMins", 1);
        }
        else if (filterType == "ICEBERG_PARTS") {
            filters.icebergParts_.limit = safeStoi(filter, "limit");
        }
        else if (filterType == "MAX_NUM_ORDERS") {
            filters.maxNumOrders_.maxNumOrders = safeStoi(filter, "maxNumOrders");
        }
        else if (filterType == "MAX_NUM_ALGO_ORDERS") {
            filters.maxNumAlgoOrders_.maxNumAlgoOrders = safeStoi(filter, "maxNumAlgoOrders");
        }
        else if (filterType == "MAX_NUM_ICEBERG_ORDERS") {
            filters.maxNumIcebergOrders_.maxNumIcebergOrders = safeStoi(filter, "maxNumIcebergOrders");
        }
        else if (filterType == "MAX_POSITION") {
            filters.maxPosition_.maxPosition = safeStod(filter, "maxPosition");
        }
        else if (filterType == "TRAILING_DELTA") {
            filters.trailingDelta_.minTrailingAboveDelta = safeStol(filter, "minTrailingAboveDelta");
            filters.trailingDelta_.maxTrailingAboveDelta = safeStol(filter, "maxTrailingAboveDelta");
            filters.trailingDelta_.minTrailingBelowDelta = safeStol(filter, "minTrailingBelowDelta");
            filters.trailingDelta_.maxTrailingBelowDelta = safeStol(filter, "maxTrailingBelowDelta");
        }
        else if (filterType == "MAX_NUM_ORDER_AMENDS") {
            filters.maxNumOrderAmends_.maxNumOrderAmends = safeStoi(filter, "maxNumOrderAmends");
        }
        else if (filterType == "MAX_NUM_ORDER_LISTS") {
            filters.maxNumOrderLists_.maxNumOrderLists = safeStoi(filter, "maxNumOrderLists");
        }
    }

    return filters;
}
