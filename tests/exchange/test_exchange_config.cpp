#include <gtest/gtest.h>
#include <exchange/exchange_config.h>

namespace funding {
namespace testing {

class ExchangeConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up common test data
    }
};

TEST_F(ExchangeConfigTest, ConstructorSetsValuesCorrectly) {
    // Create an exchange config with test values
    ExchangeConfig config("binance", "api_key_123", "api_secret_456", false);
    
    // Verify the values were set correctly
    EXPECT_EQ("binance", config.getExchangeName());
    EXPECT_EQ("api_key_123", config.getApiKey());
    EXPECT_EQ("api_secret_456", config.getApiSecret());
    EXPECT_FALSE(config.getUseTestnet());
}

TEST_F(ExchangeConfigTest, ExtraParamsAreAccessible) {
    // Create a map of extra parameters
    std::map<std::string, std::string> params = {
        {"passphrase", "secure_123"},
        {"subaccount", "trading_bot_1"}
    };
    
    // Create config with extra parameters
    ExchangeConfig config("kucoin", "key", "secret", true, params);
    
    // Verify parameters can be accessed
    EXPECT_EQ("secure_123", config.getParam("passphrase"));
    EXPECT_EQ("trading_bot_1", config.getParam("subaccount"));
    
    // Non-existent parameter should return empty string or default
    EXPECT_EQ("", config.getParam("non_existent"));
    EXPECT_EQ("default_value", config.getParam("non_existent", "default_value"));
}

TEST_F(ExchangeConfigTest, DefaultValuesAreCorrect) {
    // Create with minimal parameters
    ExchangeConfig config("exchange_name");
    
    // Verify defaults
    EXPECT_EQ("exchange_name", config.getExchangeName());
    EXPECT_EQ("", config.getApiKey());
    EXPECT_EQ("", config.getApiSecret());
    EXPECT_FALSE(config.getUseTestnet());
    EXPECT_TRUE(config.getAllParams().empty());
}

} // namespace testing
} // namespace funding 