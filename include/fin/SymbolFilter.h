#pragma once

#include <cmath>
#include <string>
#include <nlohmann/json.hpp>

class SymbolFilter {
public:
    SymbolFilter() = default;

    SymbolFilter(double minQty, double maxQty, double stepSize, int precision)
        : minQty_(minQty), maxQty_(maxQty), stepSize_(stepSize), precision_(precision) {}

    static SymbolFilter fromJson(const nlohmann::json& filters) {
        double minQty = 0, maxQty = 0, stepSize = 0;
        int precision = 0;

        for (const auto& filter : filters) {
            std::string filterType = filter["filterType"].get<std::string>();
            if (filterType == "LOT_SIZE") {
                minQty = std::stod(filter["minQty"].get<std::string>());
                maxQty = std::stod(filter["maxQty"].get<std::string>());
                stepSize = std::stod(filter["stepSize"].get<std::string>());

                if (stepSize > 0) {
                    precision = static_cast<int>(-std::log10(stepSize));
                }
            }
        }

        return SymbolFilter(minQty, maxQty, stepSize, precision);
    }

    double roundQty(double qty) const {
        if (stepSize_ <= 0) return qty;
        double rounded = std::floor(qty / stepSize_) * stepSize_;
        return std::max(minQty_, std::min(rounded, maxQty_));
    }

    double getMinQty() const { return minQty_; }
    double getMaxQty() const { return maxQty_; }
    double getStepSize() const { return stepSize_; }
    int getPrecision() const { return precision_; }

private:
    double minQty_ = 0;
    double maxQty_ = 0;
    double stepSize_ = 0;
    int precision_ = 0;
};
