#pragma once

#include <memory>
#include <map>
#include <vector>
#include <string>

#include "rest/ApiClient.hpp"
#include "crypto/ed25519.hpp"
#include "fin/Symbol.h"
#include "fin/SymbolFilters.h"

// Admin handles REST-based administrative operations:
// - Exchange info (symbol list, filters)
// - Account information (balances)
class Admin {
public:
    Admin(const std::string& endpoint, const std::string& apiKey, crypto::ed25519& key);
    ~Admin() = default;

    // Fetch all tradeable symbols with their filters
    std::vector<fin::Symbol> fetchExchangeInfo();

    // Fetch account balances (non-zero only)
    std::map<std::string, double> fetchAccountBalances();

    // Access underlying REST client if needed
    BNB::REST::ApiClient& restClient() { return *restClient_; }

private:
    std::unique_ptr<BNB::REST::ApiClient> restClient_;
};
