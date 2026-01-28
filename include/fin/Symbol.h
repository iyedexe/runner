#pragma once
#include <string>
#include "fin/SymbolFilters.h"

namespace fin {

class Symbol {
private:
    std::string baseAsset_;
    std::string quoteAsset_;
    std::string symbol_;
    SymbolFilters filters_;

public:
    Symbol(const std::string& base, const std::string& quote, const std::string& symbol, const SymbolFilters& filters)
        : baseAsset_(base), quoteAsset_(quote), symbol_(symbol), filters_(filters) {}

    const std::string& getSymbol() const { return symbol_; }
    const std::string& getQuote() const { return quoteAsset_; }
    const std::string& getBase() const { return baseAsset_; }
    const SymbolFilters& getFilters() const { return filters_; }

    bool operator==(const Symbol& other) const {
        return baseAsset_ == other.baseAsset_ && quoteAsset_ == other.quoteAsset_;
    }

    bool operator!=(const Symbol& other) const {
        return !(*this == other);
    }

    std::string to_str() const { return symbol_; }
};

} // namespace fin
