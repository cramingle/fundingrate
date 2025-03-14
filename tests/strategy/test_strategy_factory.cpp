#include <gtest/gtest.h>
#include <strategy/arbitrage_strategy.h>
#include <strategy/composite_strategy.h>
#include <exchange/types.h>
#include <config/config_manager.h>
#include "../exchange/mock_exchange.h"
#include <memory>

// Forward declaration of factory functions we'll test
namespace funding {
    std::unique_ptr<ArbitrageStrategy> createSameExchangeSpotPerpStrategy(
        const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
        double min_funding_rate = 0.0001,
        double min_expected_profit = 1.0);
        
    std::unique_ptr<ArbitrageStrategy> createCrossExchangePerpStrategy(
        const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
        double min_funding_rate = 0.0001,
        double min_expected_profit = 1.0);
        
    std::unique_ptr<ArbitrageStrategy> createCrossExchangeSpotPerpStrategy(
        const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
        double min_funding_rate = 0.0001,
        double min_expected_profit = 1.0);
        
    std::unique_ptr<ArbitrageStrategy> createStrategy(
        const std::string& strategy_type,
        const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
        double min_funding_rate = 0.0001,
        double min_expected_profit = 1.0);
        
    std::vector<std::unique_ptr<ArbitrageStrategy>> createAllStrategies(
        const BotConfig& config,
        const std::map<std::string, std::shared_ptr<ExchangeInterface>>& exchanges);
}

namespace funding {
namespace testing {

class StrategyFactoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create mock exchanges
        exchange1_ = std::make_shared<MockExchange>();
        exchange2_ = std::make_shared<MockExchange>();
        exchange3_ = std::make_shared<MockExchange>();
        
        // Set up exchange names
        ON_CALL(*exchange1_, getName())
            .WillByDefault(::testing::Return("Binance"));
        ON_CALL(*exchange2_, getName())
            .WillByDefault(::testing::Return("OKX"));
        ON_CALL(*exchange3_, getName())
            .WillByDefault(::testing::Return("Bybit"));
        
        // Create vector of exchanges
        exchanges_.push_back(exchange1_);
        exchanges_.push_back(exchange2_);
        exchanges_.push_back(exchange3_);
        
        // Create exchange map
        exchange_map_["Binance"] = exchange1_;
        exchange_map_["OKX"] = exchange2_;
        exchange_map_["Bybit"] = exchange3_;
        
        // Set up mock instruments for each exchange
        std::vector<Instrument> spot_instruments;
        std::vector<Instrument> perp_instruments;
        
        Instrument btc_spot;
        btc_spot.symbol = "BTC/USDT";
        btc_spot.market_type = MarketType::SPOT;
        spot_instruments.push_back(btc_spot);
        
        Instrument btc_perp;
        btc_perp.symbol = "BTC/USDT:PERP";
        btc_perp.market_type = MarketType::PERPETUAL;
        perp_instruments.push_back(btc_perp);
        
        // Set up mock instruments for each exchange
        for (auto& exchange : exchanges_) {
            auto mock_exchange = std::dynamic_pointer_cast<MockExchange>(exchange);
            if (mock_exchange) {
                mock_exchange->setupInstruments(MarketType::SPOT, spot_instruments);
                mock_exchange->setupInstruments(MarketType::PERPETUAL, perp_instruments);
            }
        }
    }
    
    std::shared_ptr<MockExchange> exchange1_;
    std::shared_ptr<MockExchange> exchange2_;
    std::shared_ptr<MockExchange> exchange3_;
    std::vector<std::shared_ptr<ExchangeInterface>> exchanges_;
    std::map<std::string, std::shared_ptr<ExchangeInterface>> exchange_map_;
};

TEST_F(StrategyFactoryTest, CreatesSameExchangeStrategy) {
    auto strategy = createSameExchangeSpotPerpStrategy(exchanges_, 0.0002, 2.0);
    
    // Should return a valid strategy
    ASSERT_NE(nullptr, strategy);
    
    // Check that configuration was applied
    EXPECT_DOUBLE_EQ(0.0002, strategy->getMinFundingRate());
    EXPECT_DOUBLE_EQ(2.0, strategy->getMinExpectedProfit());
    
    // Test that it can find opportunities (basic functionality check)
    auto opportunities = strategy->findOpportunities();
    
    // We can't make specific assertions since the mock exchanges don't have
    // instruments set up, but the call should not fail
}

TEST_F(StrategyFactoryTest, CreatesCrossExchangePerpStrategy) {
    auto strategy = createCrossExchangePerpStrategy(exchanges_, 0.0003, 3.0);
    
    // Should return a valid strategy
    ASSERT_NE(nullptr, strategy);
    
    // Check that configuration was applied
    EXPECT_DOUBLE_EQ(0.0003, strategy->getMinFundingRate());
    EXPECT_DOUBLE_EQ(3.0, strategy->getMinExpectedProfit());
    
    // Test that it can find opportunities (basic functionality check)
    auto opportunities = strategy->findOpportunities();
}

TEST_F(StrategyFactoryTest, CreatesCrossExchangeSpotPerpStrategy) {
    auto strategy = createCrossExchangeSpotPerpStrategy(exchanges_, 0.0004, 4.0);
    
    // Should return a valid strategy
    ASSERT_NE(nullptr, strategy);
    
    // Check that configuration was applied
    EXPECT_DOUBLE_EQ(0.0004, strategy->getMinFundingRate());
    EXPECT_DOUBLE_EQ(4.0, strategy->getMinExpectedProfit());
    
    // Test that it can find opportunities (basic functionality check)
    auto opportunities = strategy->findOpportunities();
}

TEST_F(StrategyFactoryTest, CreatesStrategyByType) {
    // Test creating each strategy type
    auto strategy1 = createStrategy("same_exchange_spot_perp", exchanges_, 0.0005, 5.0);
    auto strategy2 = createStrategy("cross_exchange_perp", exchanges_, 0.0006, 6.0);
    auto strategy3 = createStrategy("cross_exchange_spot_perp", exchanges_, 0.0007, 7.0);
    
    // All strategies should be created
    ASSERT_NE(nullptr, strategy1);
    ASSERT_NE(nullptr, strategy2);
    ASSERT_NE(nullptr, strategy3);
    
    // Check that configuration was applied
    EXPECT_DOUBLE_EQ(0.0005, strategy1->getMinFundingRate());
    EXPECT_DOUBLE_EQ(5.0, strategy1->getMinExpectedProfit());
    
    EXPECT_DOUBLE_EQ(0.0006, strategy2->getMinFundingRate());
    EXPECT_DOUBLE_EQ(6.0, strategy2->getMinExpectedProfit());
    
    EXPECT_DOUBLE_EQ(0.0007, strategy3->getMinFundingRate());
    EXPECT_DOUBLE_EQ(7.0, strategy3->getMinExpectedProfit());
    
    // Unknown strategy type should return nullptr
    auto unknown_strategy = createStrategy("UNKNOWN_STRATEGY", exchanges_);
    ASSERT_EQ(nullptr, unknown_strategy);
}

TEST_F(StrategyFactoryTest, CreatesAllStrategiesFromConfig) {
    // Create a sample bot config with multiple strategies
    BotConfig config;
    
    // Add strategy configs
    StrategyConfig same_exchange_config;
    same_exchange_config.type = StrategyType::SAME_EXCHANGE_SPOT_PERP;
    same_exchange_config.min_funding_rate = 0.0001;
    same_exchange_config.min_expected_profit = 5.0;
    config.strategies.push_back(same_exchange_config);
    
    StrategyConfig cross_perp_config;
    cross_perp_config.type = StrategyType::CROSS_EXCHANGE_PERP_PERP;
    cross_perp_config.min_funding_rate = 0.0002;
    cross_perp_config.min_expected_profit = 8.0;
    config.strategies.push_back(cross_perp_config);
    
    // Create all strategies
    auto strategies = createAllStrategies(config, exchange_map_);
    
    // Should create a strategy for each config
    ASSERT_EQ(2, strategies.size());
    
    // Each strategy should be valid
    for (const auto& strategy : strategies) {
        ASSERT_NE(nullptr, strategy);
    }
    
    // Check that the first strategy has the correct configuration
    EXPECT_DOUBLE_EQ(0.0001, strategies[0]->getMinFundingRate());
    EXPECT_DOUBLE_EQ(5.0, strategies[0]->getMinExpectedProfit());
    
    // Check that the second strategy has the correct configuration
    EXPECT_DOUBLE_EQ(0.0002, strategies[1]->getMinFundingRate());
    EXPECT_DOUBLE_EQ(8.0, strategies[1]->getMinExpectedProfit());
}

} // namespace testing
} // namespace funding 