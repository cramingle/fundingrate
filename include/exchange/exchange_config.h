#pragma once

#include <string>
#include <map>

namespace funding {

// Configuration for exchange connections
class ExchangeConfig {
public:
    ExchangeConfig(
        const std::string& exchange_name,
        const std::string& api_key = "",
        const std::string& api_secret = "",
        bool use_testnet = false,
        const std::map<std::string, std::string>& extra_params = {}
    ) : 
        exchange_name_(exchange_name),
        api_key_(api_key),
        api_secret_(api_secret),
        use_testnet_(use_testnet),
        extra_params_(extra_params) {}
    
    // Getters
    std::string getExchangeName() const { return exchange_name_; }
    std::string getApiKey() const { return api_key_; }
    std::string getApiSecret() const { return api_secret_; }
    bool getUseTestnet() const { return use_testnet_; }
    
    // Get extra parameter by key
    std::string getParam(const std::string& key, const std::string& default_value = "") const {
        auto it = extra_params_.find(key);
        if (it != extra_params_.end()) {
            return it->second;
        }
        return default_value;
    }
    
    // Get all extra parameters
    const std::map<std::string, std::string>& getAllParams() const {
        return extra_params_;
    }

private:
    std::string exchange_name_;
    std::string api_key_;
    std::string api_secret_;
    bool use_testnet_;
    std::map<std::string, std::string> extra_params_;
};

} // namespace funding 