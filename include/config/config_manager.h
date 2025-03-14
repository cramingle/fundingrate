#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <risk/risk_manager.h>

namespace funding {

// Forward declaration
class ExchangeInterface;

// Exchange configuration
struct ExchangeConfig {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;   // Some exchanges require this
    std::string base_url;
    bool use_testnet;
    int connect_timeout_ms;
    int request_timeout_ms;
};

// Supported arbitrage strategy types
enum class StrategyType {
    SAME_EXCHANGE_SPOT_PERP,
    CROSS_EXCHANGE_PERP_PERP,
    CROSS_EXCHANGE_SPOT_PERP
};

// Strategy configuration
struct StrategyConfig {
    StrategyType type;
    double min_funding_rate;                   // Minimum funding rate to consider
    double min_expected_profit;                // Minimum expected profit
    int scan_interval_seconds;                 // How often to scan for opportunities
};

// Main bot configuration
struct BotConfig {
    std::string bot_name;
    bool simulation_mode;
    int log_level;
    std::string log_file;
    std::map<std::string, ExchangeConfig> exchanges;  // Map of exchange name to config
    std::vector<StrategyConfig> strategies;
    RiskConfig risk_config;
};

// Handles loading, validating, and providing access to configuration
class ConfigManager {
public:
    ConfigManager(const std::string& config_file);
    
    // Load configuration from file
    bool loadConfig();
    
    // Save current configuration to file
    bool saveConfig();
    
    // Get configuration
    const BotConfig& getBotConfig() const;
    ExchangeConfig getExchangeConfig(const std::string& exchange_name) const;
    RiskConfig getRiskConfig() const;
    
    // Create exchange instances from configuration
    std::map<std::string, std::shared_ptr<ExchangeInterface>> createExchanges();
    
    // Validate configuration
    bool validateConfig();
    
private:
    std::string config_file_;
    BotConfig config_;
};

} // namespace funding 