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
        
        // Parse JSON
        nlohmann::json json_config;
        try {
            file >> json_config;
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "Failed to parse config file: " << e.what() << std::endl;
            return false;
        }
        
        // Load general settings
        if (json_config.contains("bot_name")) {
            config_.bot_name = json_config["bot_name"];
        }
        if (json_config.contains("simulation_mode")) {
            config_.simulation_mode = json_config["simulation_mode"];
        }
        if (json_config.contains("log_level")) {
            config_.log_level = json_config["log_level"];
        }
        if (json_config.contains("log_file")) {
            config_.log_file = json_config["log_file"];
        }
        
        // Load risk config
        if (json_config.contains("risk_config")) {
            auto& risk = json_config["risk_config"];
            if (risk.contains("max_position_size_usd")) {
                config_.risk_config.max_position_size_usd = risk["max_position_size_usd"];
            }
            if (risk.contains("max_total_position_usd")) {
                config_.risk_config.max_total_position_usd = risk["max_total_position_usd"];
            }
            if (risk.contains("max_position_per_exchange")) {
                config_.risk_config.max_position_per_exchange = risk["max_position_per_exchange"];
            }
            if (risk.contains("max_price_divergence_pct")) {
                config_.risk_config.max_price_divergence_pct = risk["max_price_divergence_pct"];
            }
            if (risk.contains("target_profit_pct")) {
                config_.risk_config.target_profit_pct = risk["target_profit_pct"];
            }
            if (risk.contains("stop_loss_pct")) {
                config_.risk_config.stop_loss_pct = risk["stop_loss_pct"];
            }
            if (risk.contains("dynamic_position_sizing")) {
                config_.risk_config.dynamic_position_sizing = risk["dynamic_position_sizing"];
            }
            if (risk.contains("min_liquidity_depth")) {
                config_.risk_config.min_liquidity_depth = risk["min_liquidity_depth"];
            }
        }
        
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