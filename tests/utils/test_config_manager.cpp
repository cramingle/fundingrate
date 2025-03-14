#include <gtest/gtest.h>
#include <utils/config_manager.h>
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
            "general": {
                "log_level": "info",
                "data_dir": "/tmp/funding_data"
            },
            "exchanges": [
                {
                    "name": "binance",
                    "api_key": "test_key_1",
                    "api_secret": "test_secret_1",
                    "testnet": true,
                    "extra_params": {
                        "use_proxy": true,
                        "proxy_url": "http://proxy.example.com:8080"
                    }
                },
                {
                    "name": "kucoin",
                    "api_key": "test_key_2",
                    "api_secret": "test_secret_2",
                    "testnet": false
                }
            ],
            "strategies": [
                {
                    "type": "same_exchange_spot_perp",
                    "exchange": "binance",
                    "min_funding_rate": 0.0005,
                    "min_profit": 1.0
                },
                {
                    "type": "cross_exchange_perp",
                    "exchange1": "binance",
                    "exchange2": "kucoin",
                    "min_price_difference": 0.002
                }
            ],
            "risk": {
                "max_position_size_usd": 1000.0,
                "max_total_position_usd": 5000.0,
                "max_position_per_exchange": 0.5,
                "target_profit_pct": 2.0,
                "stop_loss_pct": 1.0
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
    ConfigManager config_manager;
    bool success = config_manager.loadConfigFromFile(temp_config_file_);
    
    // Verify successful loading
    EXPECT_TRUE(success);
    
    // Check general settings
    auto general_config = config_manager.getGeneralConfig();
    EXPECT_EQ("info", general_config.log_level);
    EXPECT_EQ("/tmp/funding_data", general_config.data_dir);
    
    // Check exchanges count
    auto exchanges = config_manager.getExchangeConfigs();
    EXPECT_EQ(2, exchanges.size());
    
    // Check specific exchange details
    EXPECT_EQ("binance", exchanges[0].name);
    EXPECT_EQ("test_key_1", exchanges[0].api_key);
    EXPECT_EQ("test_secret_1", exchanges[0].api_secret);
    EXPECT_TRUE(exchanges[0].testnet);
    
    // Check extra params
    EXPECT_TRUE(exchanges[0].hasExtraParam("use_proxy"));
    EXPECT_EQ("true", exchanges[0].getExtraParam("use_proxy"));
    EXPECT_EQ("http://proxy.example.com:8080", exchanges[0].getExtraParam("proxy_url"));
    
    // Check second exchange
    EXPECT_EQ("kucoin", exchanges[1].name);
    EXPECT_FALSE(exchanges[1].testnet);
    
    // Check strategies
    auto strategies = config_manager.getStrategyConfigs();
    EXPECT_EQ(2, strategies.size());
    
    // Check first strategy
    EXPECT_EQ("same_exchange_spot_perp", strategies[0].type);
    EXPECT_EQ("binance", strategies[0].exchange);
    EXPECT_NEAR(0.0005, strategies[0].min_funding_rate, 0.00001);
    
    // Check risk settings
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
    
    ConfigManager config_manager;
    bool success = config_manager.loadConfigFromFile(temp_config_file_);
    
    // Should fail to load
    EXPECT_FALSE(success);
}

TEST_F(ConfigManagerTest, HandlesNonexistentFile) {
    ConfigManager config_manager;
    bool success = config_manager.loadConfigFromFile("/nonexistent/file.json");
    
    // Should fail to load
    EXPECT_FALSE(success);
}

TEST_F(ConfigManagerTest, SavesConfigToFile) {
    // First load a config
    temp_config_file_ = createTempConfigFile(basic_config_json_);
    
    ConfigManager config_manager;
    config_manager.loadConfigFromFile(temp_config_file_);
    
    // Now save to a new file
    std::string output_file = "/tmp/test_config_output.json";
    bool success = config_manager.saveConfigToFile(output_file);
    
    // Verify success
    EXPECT_TRUE(success);
    
    // Verify the file exists
    std::ifstream file(output_file);
    EXPECT_TRUE(file.good());
    
    // Clean up
    file.close();
    std::remove(output_file.c_str());
}

TEST_F(ConfigManagerTest, ModifiesAndPersistsConfig) {
    // Create a temporary config file
    temp_config_file_ = createTempConfigFile(basic_config_json_);
    
    // Load the config
    ConfigManager config_manager;
    config_manager.loadConfigFromFile(temp_config_file_);
    
    // Modify exchange config
    auto exchanges = config_manager.getExchangeConfigs();
    exchanges[0].api_key = "modified_key";
    exchanges[0].setExtraParam("new_param", "new_value");
    config_manager.updateExchangeConfigs(exchanges);
    
    // Modify risk config
    auto risk_config = config_manager.getRiskConfig();
    risk_config.max_position_size_usd = 2000.0;
    config_manager.updateRiskConfig(risk_config);
    
    // Save to a new file
    std::string output_file = "/tmp/test_config_modified.json";
    config_manager.saveConfigToFile(output_file);
    
    // Load the modified config in a new instance
    ConfigManager new_config_manager;
    new_config_manager.loadConfigFromFile(output_file);
    
    // Verify modifications
    auto new_exchanges = new_config_manager.getExchangeConfigs();
    EXPECT_EQ("modified_key", new_exchanges[0].api_key);
    EXPECT_EQ("new_value", new_exchanges[0].getExtraParam("new_param"));
    
    auto new_risk_config = new_config_manager.getRiskConfig();
    EXPECT_NEAR(2000.0, new_risk_config.max_position_size_usd, 0.01);
    
    // Clean up
    std::remove(output_file.c_str());
}

} // namespace testing
} // namespace funding 