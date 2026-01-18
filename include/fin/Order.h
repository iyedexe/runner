#pragma once
#include "fin/Symbol.h"


enum class Way {
    BUY,
    SELL,
    HOLD
};

enum class OrderType {
    MARKET,
    LIMIT
};

class Order {
private:
    ::Symbol _symbol;
    Way _way;
    OrderType _type;
    double _quantity;
    double _price;

public:
    Order(const ::Symbol& symbol, Way way, OrderType type = OrderType::MARKET, double quantity = 0.0, double price = 0.0)
        : _symbol(symbol), _way(way), _type(type), _quantity(quantity), _price(price) {}

    Way getWay() const { return _way; }
    ::Symbol getSymbol() const { return _symbol; }

    double getQty() const { return _quantity; }
    void setQty(double value) { _quantity = value; }

    OrderType getType() const { return _type; }
    void setType(OrderType value) { _type = value; }

    double getPrice() const { return _price; }
    void setPrice(double value) { _price = value; }

    std::string getStartingAsset() const { return (_way == Way::BUY) ? _symbol.getQuote() : _symbol.getBase();}
    std::string getResultingAsset() const { return (_way == Way::BUY) ? _symbol.getBase() : _symbol.getQuote();}

    std::string to_str() const {
        std::string wayStr= (static_cast<int>(_way)==0) ? "BUY" : "SELL";
        return wayStr + "@" + _symbol.to_str();
    }
};
