#pragma once

#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "fin/Symbol.h"

class ExchangeInfo {
public:
    ExchangeInfo(const nlohmann::json& response) {
        if (response.contains("result") && response["result"].contains("symbols")) {
            parseSymbols(response["result"]["symbols"]);
        }
    }

    std::vector<fin::Symbol> getSymbols() const {
        return symbols_;
    }

private:
    void parseSymbols(const nlohmann::json& symbolsJson) {
        for (const auto& sym : symbolsJson) {
            std::string symbol = sym["symbol"].get<std::string>();
            std::string baseAsset = sym["baseAsset"].get<std::string>();
            std::string quoteAsset = sym["quoteAsset"].get<std::string>();

            SymbolFilters filters;
            if (sym.contains("filters")) {
                filters = SymbolFilters::fromJson(sym["filters"]);
            }

            symbols_.emplace_back(baseAsset, quoteAsset, symbol, filters);
        }
    }

    std::vector<fin::Symbol> symbols_;
};
