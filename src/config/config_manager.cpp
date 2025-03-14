#include <config/config_manager.h>
#include <exchange/exchange_interface.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace funding {

ConfigManager::ConfigManager(const std::string& config_file)
    : config_file_(config_file) {
    // Initialize with default values
    config_.bot_name = "FundingRateArbitrageBot";
    config_.simulation_mode = true;
    config_.log_level = 2; // INFO level
    config_.log_file = "logs/funding_bot.log";
    
    // Default risk config
    config_.risk_config.max_position_size_usd = 1000.0;
    config_.risk_config.max_total_position_usd = 5000.0;
    config_.risk_config.max_position_per_exchange = 0.25;
    config_.risk_config.max_price_divergence_pct = 1.0;
    config_.risk_config.target_profit_pct = 0.5;
    config_.risk_config.stop_loss_pct = 1.0;
    config_.risk_config.dynamic_position_sizing = true;
    config_.risk_config.min_liquidity_depth = 10000.0;
}

bool ConfigManager::loadConfig() {
    try {
        std::ifstream file(config_file_);
        if (!file.is_open()) {
            std::cerr << "Failed to open config file: " << config_file_ << std::endl;
            return false;
        }
        
        // In a real implementation, we would parse JSON here
        // For this example, we'll just use default values
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << std::endl;
        return false;
    }
}

bool ConfigManager::saveConfig() {
    try {
        std::ofstream file(config_file_);
        if (!file.is_open()) {
            std::cerr << "Failed to open config file for writing: " << config_file_ << std::endl;
            return false;
        }
        
        // In a real implementation, we would serialize to JSON here
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving config: " << e.what() << std::endl;
        return false;
    }
}

const BotConfig& ConfigManager::getBotConfig() const {
    return config_;
}

ExchangeConfig ConfigManager::getExchangeConfig(const std::string& exchange_name) const {
    auto it = config_.exchanges.find(exchange_name);
    if (it != config_.exchanges.end()) {
        return it->second;
    }
    
    // Return default config if not found
    ExchangeConfig default_config;
    default_config.api_key = "";
    default_config.api_secret = "";
    default_config.passphrase = "";
    default_config.base_url = "";
    default_config.use_testnet = true;
    default_config.connect_timeout_ms = 5000;
    default_config.request_timeout_ms = 10000;
    
    return default_config;
}

RiskConfig ConfigManager::getRiskConfig() const {
    return config_.risk_config;
}

std::map<std::string, std::shared_ptr<ExchangeInterface>> ConfigManager::createExchanges() {
    std::map<std::string, std::shared_ptr<ExchangeInterface>> exchanges;
    
    // In a real implementation, we would create exchange instances based on config
    // For this example, we'll just return an empty map
    
    return exchanges;
}

bool ConfigManager::validateConfig() {
    // In a real implementation, we would validate the config here
    return true;
}

} // namespace funding 