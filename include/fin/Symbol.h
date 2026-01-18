#pragma once
#include <string>
#include "fin/SymbolFilter.h"

class Symbol {
private:
    std::string _base_asset;
    std::string _quote_asset;
    std::string symbol_;
    SymbolFilter filter_;

public:
    Symbol(const std::string& base, const std::string& quote, const std::string& symbol, const SymbolFilter& filter) 
        : _base_asset(base), _quote_asset(quote) , symbol_(symbol), filter_(filter){
    }

    std::string getSymbol() const { return symbol_; }
    std::string getQuote() const { return _quote_asset; }
    std::string getBase() const { return _base_asset; }

    void setFilter(SymbolFilter filter) {filter_ = filter;};
    SymbolFilter getFilter() {return filter_;};

    bool operator==(const Symbol& other) const {
        return _base_asset == other._base_asset && _quote_asset == other._quote_asset;
    }

    bool operator!=(const Symbol& other) const {
        return _base_asset != other._base_asset || _quote_asset != other._quote_asset;
    }

    std::string to_str() const {
        return symbol_;
    }
};