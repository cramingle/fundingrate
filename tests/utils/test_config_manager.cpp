#include <gtest/gtest.h>
#include <config/config_manager.h>
#include <fstream>
#include <string>

namespace funding {
namespace testing {

// Helper function to create a temporary config file
std::string createTempConfigFile(const std::string& content) {
    std::string filename = "/tmp/test_config_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".json";
    std::ofstream file(filename);
    file << content;
    file.close();
    return filename;
}

class ConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a basic config string
        basic_config_json_ = R"({
            "bot_name": "TestBot",
            "simulation_mode": true,
            "log_level": 2,
            "log_file": "logs/test_bot.log",
            "exchanges": {
                "binance": {
                    "api_key": "test_key_1",
                    "api_secret": "test_secret_1",
                    "passphrase": "",
                    "base_url": "https://api.binance.com",
                    "use_testnet": true,
                    "connect_timeout_ms": 5000,
                    "request_timeout_ms": 10000
                },
                "kucoin": {
                    "api_key": "test_key_2",
                    "api_secret": "test_secret_2",
                    "passphrase": "test_passphrase",
                    "base_url": "https://api.kucoin.com",
                    "use_testnet": false,
                    "connect_timeout_ms": 5000,
                    "request_timeout_ms": 10000
                }
            },
            "strategies": [
                {
                    "type": 0,
                    "min_funding_rate": 0.0005,
                    "min_expected_profit": 1.0,
                    "scan_interval_seconds": 300
                },
                {
                    "type": 1,
                    "min_funding_rate": 0.0002,
                    "min_expected_profit": 0.5,
                    "scan_interval_seconds": 600
                }
            ],
            "risk_config": {
                "max_position_size_usd": 1000.0,
                "max_total_position_usd": 5000.0,
                "max_position_per_exchange": 0.5,
                "max_price_divergence_pct": 0.5,
                "target_profit_pct": 2.0,
                "stop_loss_pct": 1.0,
                "dynamic_position_sizing": true,
                "min_liquidity_depth": 50000.0
            }
        })";
    }

    void TearDown() override {
        // Clean up any temp files if needed
        if (!temp_config_file_.empty()) {
            std::remove(temp_config_file_.c_str());
        }
    }

    std::string basic_config_json_;
    std::string temp_config_file_;
};

TEST_F(ConfigManagerTest, LoadsConfigFromFile) {
    // Create a temporary config file
    temp_config_file_ = createTempConfigFile(basic_config_json_);
    
    // Create and load the config manager
    ConfigManager config_manager(temp_config_file_);
    bool success = config_manager.loadConfig();
    
    // Verify successful loading
    EXPECT_TRUE(success);
    
    // Check general settings
    const auto& bot_config = config_manager.getBotConfig();
    EXPECT_EQ("TestBot", bot_config.bot_name);
    EXPECT_TRUE(bot_config.simulation_mode);
    EXPECT_EQ(2, bot_config.log_level);
    
    // Check risk config
    auto risk_config = config_manager.getRiskConfig();
    EXPECT_NEAR(1000.0, risk_config.max_position_size_usd, 0.01);
    EXPECT_NEAR(5000.0, risk_config.max_total_position_usd, 0.01);
    EXPECT_NEAR(0.5, risk_config.max_position_per_exchange, 0.01);
    EXPECT_NEAR(2.0, risk_config.target_profit_pct, 0.01);
    EXPECT_NEAR(1.0, risk_config.stop_loss_pct, 0.01);
}

TEST_F(ConfigManagerTest, HandlesInvalidConfigFile) {
    // Create a file with invalid JSON
    temp_config_file_ = createTempConfigFile("{ invalid json }");
    
    ConfigManager config_manager(temp_config_file_);
    bool success = config_manager.loadConfig();
    
    // Should fail to load
    EXPECT_FALSE(success);
}

TEST_F(ConfigManagerTest, HandlesNonexistentFile) {
    ConfigManager config_manager("/nonexistent/file.json");
    bool success = config_manager.loadConfig();
    
    // Should fail to load
    EXPECT_FALSE(success);
}

TEST_F(ConfigManagerTest, SavesConfigToFile) {
    // First load a config
    temp_config_file_ = createTempConfigFile(basic_config_json_);
    
    ConfigManager config_manager(temp_config_file_);
    config_manager.loadConfig();
    
    // Now save to a new file
    std::string output_file = "/tmp/test_config_output.json";
    bool success = config_manager.saveConfig();
    
    // Verify success
    EXPECT_TRUE(success);
    
    // Clean up
    std::remove(output_file.c_str());
}

} // namespace testing
} // namespace funding 