#include "market_connection/Admin.h"
#include "rest/requests/endpoints/General.hpp"
#include "rest/requests/endpoints/Account.hpp"
#include "logger.hpp"

Admin::Admin(const std::string& endpoint, const std::string& apiKey, crypto::ed25519& key)
    : restClient_(std::make_unique<BNB::REST::ApiClient>(endpoint, apiKey, key))
{
    LOG_INFO("[Admin] Created REST client for: {}", endpoint);
}

std::vector<fin::Symbol> Admin::fetchExchangeInfo() {
    LOG_INFO("[Admin] Fetching exchange info from REST API...");

    nlohmann::json response = restClient_->sendRequest(
        BNB::REST::Endpoints::General::ExchangeInfo()
            .permissions({"SPOT"})
    );

    if (!response.contains("symbols")) {
        throw std::runtime_error("Exchange info response missing 'symbols' field");
    }

    std::vector<fin::Symbol> result;

    for (const auto& data : response["symbols"]) {
        if (!data.contains("symbol") || !data.contains("filters")) {
            continue;
        }
        if (data.value("status", "") != "TRADING") {
            continue;
        }

        std::string baseAsset = data.value("baseAsset", "");
        std::string quoteAsset = data.value("quoteAsset", "");
        if (baseAsset.empty() || quoteAsset.empty()) {
            continue;
        }

        result.emplace_back(
            baseAsset,
            quoteAsset,
            data["symbol"].get<std::string>(),
            SymbolFilters::fromJson(data["filters"])
        );
    }

    LOG_INFO("[Admin] Fetched {} symbols from exchange info", result.size());
    return result;
}

std::map<std::string, double> Admin::fetchAccountBalances() {
    LOG_INFO("[Admin] Fetching account balances from REST API...");

    std::map<std::string, double> balances;

    try {
        nlohmann::json response = restClient_->sendRequest(
            BNB::REST::Endpoints::Account::AccountInformation()
                .omitZeroBalances(true)
        );

        if (!response.contains("balances")) {
            LOG_WARNING("[Admin] Account response missing 'balances' field");
            return balances;
        }

        for (const auto& bal : response["balances"]) {
            std::string asset = bal.value("asset", "");
            double free = 0.0;

            // Handle both string and number formats
            if (bal.contains("free")) {
                if (bal["free"].is_string()) {
                    free = std::stod(bal["free"].get<std::string>());
                } else {
                    free = bal["free"].get<double>();
                }
            }

            if (!asset.empty() && free > 0) {
                balances[asset] = free;
                LOG_DEBUG("[Admin] Balance: {} = {}", asset, free);
            }
        }

        LOG_INFO("[Admin] Loaded {} non-zero balances", balances.size());

    } catch (const std::exception& e) {
        LOG_ERROR("[Admin] Failed to fetch account balances: {}", e.what());
    }

    return balances;
}
