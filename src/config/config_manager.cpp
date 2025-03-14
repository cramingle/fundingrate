#include <config/config_manager.h>
#include <exchange/exchange_interface.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace funding {

// Forward declarations for exchange factory functions
std::shared_ptr<ExchangeInterface> createBinanceExchange(const ExchangeConfig& config);
std::shared_ptr<ExchangeInterface> createBybitExchange(const ExchangeConfig& config);
std::shared_ptr<ExchangeInterface> createOKXExchange(const ExchangeConfig& config);
std::shared_ptr<ExchangeInterface> createKuCoinExchange(const ExchangeConfig& config);
std::shared_ptr<ExchangeInterface> createBitgetExchange(const ExchangeConfig& config);

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
        
        // Serialize the config to JSON
        nlohmann::json json_config;
        
        // General settings
        json_config["bot_name"] = config_.bot_name;
        json_config["simulation_mode"] = config_.simulation_mode;
        json_config["log_level"] = config_.log_level;
        json_config["log_file"] = config_.log_file;
        
        // Risk config
        nlohmann::json risk_config;
        risk_config["max_position_size_usd"] = config_.risk_config.max_position_size_usd;
        risk_config["max_total_position_usd"] = config_.risk_config.max_total_position_usd;
        risk_config["max_position_per_exchange"] = config_.risk_config.max_position_per_exchange;
        risk_config["max_price_divergence_pct"] = config_.risk_config.max_price_divergence_pct;
        risk_config["target_profit_pct"] = config_.risk_config.target_profit_pct;
        risk_config["stop_loss_pct"] = config_.risk_config.stop_loss_pct;
        risk_config["dynamic_position_sizing"] = config_.risk_config.dynamic_position_sizing;
        risk_config["min_liquidity_depth"] = config_.risk_config.min_liquidity_depth;
        json_config["risk_config"] = risk_config;
        
        // Exchange configs
        nlohmann::json exchanges_json = nlohmann::json::object();
        for (const auto& [name, exchange_config] : config_.exchanges) {
            nlohmann::json exchange_json;
            exchange_json["api_key"] = exchange_config.api_key;
            exchange_json["api_secret"] = exchange_config.api_secret;
            exchange_json["passphrase"] = exchange_config.passphrase;
            exchange_json["base_url"] = exchange_config.base_url;
            exchange_json["use_testnet"] = exchange_config.use_testnet;
            exchange_json["connect_timeout_ms"] = exchange_config.connect_timeout_ms;
            exchange_json["request_timeout_ms"] = exchange_config.request_timeout_ms;
            exchanges_json[name] = exchange_json;
        }
        json_config["exchanges"] = exchanges_json;
        
        // Strategy configs
        nlohmann::json strategies_json = nlohmann::json::array();
        for (const auto& strategy_config : config_.strategies) {
            nlohmann::json strategy_json;
            // Convert enum to string
            std::string type_str;
            switch (strategy_config.type) {
                case StrategyType::SAME_EXCHANGE_SPOT_PERP:
                    type_str = "same_exchange_spot_perp";
                    break;
                case StrategyType::CROSS_EXCHANGE_PERP_PERP:
                    type_str = "cross_exchange_perp";
                    break;
                case StrategyType::CROSS_EXCHANGE_SPOT_PERP:
                    type_str = "cross_exchange_spot_perp";
                    break;
                default:
                    type_str = "unknown";
            }
            strategy_json["type"] = type_str;
            strategy_json["min_funding_rate"] = strategy_config.min_funding_rate;
            strategy_json["min_expected_profit"] = strategy_config.min_expected_profit;
            strategy_json["scan_interval_seconds"] = strategy_config.scan_interval_seconds;
            strategies_json.push_back(strategy_json);
        }
        json_config["strategies"] = strategies_json;
        
        // Write the JSON to the file with pretty formatting
        file << json_config.dump(4);
        
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
    
    // Create exchange instances based on config
    for (const auto& [name, exchange_config] : config_.exchanges) {
        try {
            // Create exchange based on name
            if (name == "Binance") {
                exchanges[name] = createBinanceExchange(exchange_config);
            } else if (name == "Bybit") {
                exchanges[name] = createBybitExchange(exchange_config);
            } else if (name == "OKX") {
                exchanges[name] = createOKXExchange(exchange_config);
            } else if (name == "KuCoin") {
                exchanges[name] = createKuCoinExchange(exchange_config);
            } else if (name == "Bitget") {
                exchanges[name] = createBitgetExchange(exchange_config);
            } else {
                std::cerr << "Unsupported exchange: " << name << std::endl;
            }
            
            // Log successful creation
            if (exchanges[name]) {
                std::cout << "Successfully created exchange: " << name << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error creating exchange " << name << ": " << e.what() << std::endl;
        }
    }
    
    return exchanges;
}

bool ConfigManager::validateConfig() {
    bool is_valid = true;
    
    // Validate general settings
    if (config_.bot_name.empty()) {
        std::cerr << "Error: Bot name cannot be empty" << std::endl;
        is_valid = false;
    }
    
    // Validate risk config
    if (config_.risk_config.max_position_size_usd <= 0) {
        std::cerr << "Error: max_position_size_usd must be positive" << std::endl;
        is_valid = false;
    }
    
    if (config_.risk_config.max_total_position_usd <= 0) {
        std::cerr << "Error: max_total_position_usd must be positive" << std::endl;
        is_valid = false;
    }
    
    if (config_.risk_config.max_position_per_exchange <= 0 || 
        config_.risk_config.max_position_per_exchange > 1.0) {
        std::cerr << "Error: max_position_per_exchange must be between 0 and 1" << std::endl;
        is_valid = false;
    }
    
    if (config_.risk_config.max_price_divergence_pct <= 0) {
        std::cerr << "Error: max_price_divergence_pct must be positive" << std::endl;
        is_valid = false;
    }
    
    if (config_.risk_config.target_profit_pct <= 0) {
        std::cerr << "Error: target_profit_pct must be positive" << std::endl;
        is_valid = false;
    }
    
    if (config_.risk_config.stop_loss_pct <= 0) {
        std::cerr << "Error: stop_loss_pct must be positive" << std::endl;
        is_valid = false;
    }
    
    if (config_.risk_config.min_liquidity_depth <= 0) {
        std::cerr << "Error: min_liquidity_depth must be positive" << std::endl;
        is_valid = false;
    }
    
    // Validate exchanges
    if (config_.exchanges.empty()) {
        std::cerr << "Error: No exchanges configured" << std::endl;
        is_valid = false;
    } else {
        for (const auto& [name, exchange_config] : config_.exchanges) {
            // API key and secret are required for non-simulation mode
            if (!config_.simulation_mode) {
                if (exchange_config.api_key.empty()) {
                    std::cerr << "Error: API key for exchange " << name << " is empty" << std::endl;
                    is_valid = false;
                }
                
                if (exchange_config.api_secret.empty()) {
                    std::cerr << "Error: API secret for exchange " << name << " is empty" << std::endl;
                    is_valid = false;
                }
            }
            
            // Some exchanges require a passphrase
            if ((name == "KuCoin" || name == "OKX") && exchange_config.passphrase.empty() && !config_.simulation_mode) {
                std::cerr << "Error: Passphrase for exchange " << name << " is required" << std::endl;
                is_valid = false;
            }
        }
    }
    
    // Validate strategies
    if (config_.strategies.empty()) {
        std::cerr << "Error: No strategies configured" << std::endl;
        is_valid = false;
    } else {
        for (const auto& strategy_config : config_.strategies) {
            if (strategy_config.min_funding_rate <= 0) {
                std::cerr << "Error: min_funding_rate must be positive" << std::endl;
                is_valid = false;
            }
            
            if (strategy_config.min_expected_profit <= 0) {
                std::cerr << "Error: min_expected_profit must be positive" << std::endl;
                is_valid = false;
            }
            
            if (strategy_config.scan_interval_seconds <= 0) {
                std::cerr << "Error: scan_interval_seconds must be positive" << std::endl;
                is_valid = false;
            }
        }
    }
    
    return is_valid;
}

} // namespace funding 