#pragma once

#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "fin/Symbol.h"
#include "fin/SymbolFilter.h"

class ExchangeInfo {
public:
    ExchangeInfo(const nlohmann::json& response) {
        if (response.contains("result") && response["result"].contains("symbols")) {
            parseSymbols(response["result"]["symbols"]);
        }
    }

    std::vector<Symbol> getSymbols() const {
        return symbols_;
    }

private:
    void parseSymbols(const nlohmann::json& symbolsJson) {
        for (const auto& sym : symbolsJson) {
            std::string symbol = sym["symbol"].get<std::string>();
            std::string baseAsset = sym["baseAsset"].get<std::string>();
            std::string quoteAsset = sym["quoteAsset"].get<std::string>();

            SymbolFilter filter;
            if (sym.contains("filters")) {
                filter = SymbolFilter::fromJson(sym["filters"]);
            }

            symbols_.emplace_back(baseAsset, quoteAsset, symbol, filter);
        }
    }

    std::vector<Symbol> symbols_;
};
