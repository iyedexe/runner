#pragma once
#include <vector>
#include <string>
#include "fin/Order.h"

class Signal
{
public:
    Signal(std::vector<Order> aOrders, std::string aDescription, double aPnl) :
        orders(std::move(aOrders)), description(std::move(aDescription)), pnl(aPnl)
    {}
    std::vector<Order> orders;
    std::string description;
    double pnl;
};