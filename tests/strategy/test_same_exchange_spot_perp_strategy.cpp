#include <gtest/gtest.h>
#include <strategy/arbitrage_strategy.h>
#include <exchange/types.h>
#include "../exchange/mock_exchange.h"
#include <memory>

namespace funding {
namespace testing {

class SameExchangeSpotPerpStrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a mock exchange
        mock_exchange_ = std::make_shared<MockExchange>();
        
        // Set up the exchange name
        ON_CALL(*mock_exchange_, getName())
            .WillByDefault(::testing::Return("TestExchange"));
        
        // Set up strategy with the mock exchange
        strategy_ = std::make_unique<SameExchangeSpotPerpStrategy>(mock_exchange_);
        
        // Set up basic fee structure
        FeeStructure fees;
        fees.maker_fee = 0.0001;  // 0.01%
        fees.taker_fee = 0.0005;  // 0.05%
        fees.spot_maker_fee = 0.0001;
        fees.spot_taker_fee = 0.0005;
        fees.perp_maker_fee = 0.0001;
        fees.perp_taker_fee = 0.0005;
        
        mock_exchange_->setupFeeStructure(fees);
    }
    
    // Helper method to set up instruments for spot and perp markets
    void setupBasicInstruments() {
        // Create spot BTC/USDT instrument
        Instrument spot_btc;
        spot_btc.symbol = "BTC/USDT";
        spot_btc.market_type = MarketType::SPOT;
        spot_btc.base_currency = "BTC";
        spot_btc.quote_currency = "USDT";
        spot_btc.min_order_size = 0.0001;
        spot_btc.tick_size = 0.01;
        
        // Create perp BTC/USDT instrument
        Instrument perp_btc;
        perp_btc.symbol = "BTC/USDT_PERP";
        perp_btc.market_type = MarketType::PERPETUAL;
        perp_btc.base_currency = "BTC";
        perp_btc.quote_currency = "USDT";
        perp_btc.min_order_size = 0.0001;
        perp_btc.tick_size = 0.01;
        
        // Set up the instruments in the mock exchange
        mock_exchange_->setupInstruments(MarketType::SPOT, {spot_btc});
        mock_exchange_->setupInstruments(MarketType::PERPETUAL, {perp_btc});
        
        // Set up prices for our test instruments
        mock_exchange_->setupPrice("BTC/USDT", 50000.0);  // Spot price
        mock_exchange_->setupPrice("BTC/USDT_PERP", 50100.0);  // Perp price (slight premium)
        
        // Set up funding rate for perpetual
        mock_exchange_->setupFundingRate("BTC/USDT_PERP", 0.0005);  // 0.05% funding rate (positive)
        
        // Set up order books
        OrderBook spot_ob;
        spot_ob.symbol = "BTC/USDT";
        spot_ob.bids = {
            {50000.0, 1.0},
            {49900.0, 2.0},
            {49800.0, 3.0}
        };
        spot_ob.asks = {
            {50100.0, 1.0},
            {50200.0, 2.0},
            {50300.0, 3.0}
        };
        
        OrderBook perp_ob;
        perp_ob.symbol = "BTC/USDT_PERP";
        perp_ob.bids = {
            {50100.0, 1.0},
            {50000.0, 2.0},
            {49900.0, 3.0}
        };
        perp_ob.asks = {
            {50200.0, 1.0},
            {50300.0, 2.0},
            {50400.0, 3.0}
        };
        
        mock_exchange_->setupOrderBook("BTC/USDT", spot_ob);
        mock_exchange_->setupOrderBook("BTC/USDT_PERP", perp_ob);
    }
    
    std::shared_ptr<MockExchange> mock_exchange_;
    std::unique_ptr<SameExchangeSpotPerpStrategy> strategy_;
};

TEST_F(SameExchangeSpotPerpStrategyTest, FindsArbitrageOpportunitiesWithPositiveFunding) {
    // Set up our test scenario
    setupBasicInstruments();
    
    // Find opportunities
    auto opportunities = strategy_->findOpportunities();
    
    // Verify we found at least one opportunity
    ASSERT_FALSE(opportunities.empty());
    
    // Check the first opportunity
    const auto& opp = opportunities[0];
    
    // Verify it's for the correct instruments
    EXPECT_EQ("TestExchange", opp.pair.exchange1);
    EXPECT_EQ("BTC/USDT", opp.pair.symbol1);
    EXPECT_EQ(MarketType::SPOT, opp.pair.market_type1);
    EXPECT_EQ("TestExchange", opp.pair.exchange2);
    EXPECT_EQ("BTC/USDT_PERP", opp.pair.symbol2);
    EXPECT_EQ(MarketType::PERPETUAL, opp.pair.market_type2);
    
    // Verify funding rate details
    EXPECT_FLOAT_EQ(0.0, opp.funding_rate1); // Spot doesn't pay funding
    EXPECT_FLOAT_EQ(0.0005, opp.funding_rate2); // Perp funding rate
    EXPECT_GT(opp.net_funding_rate, 0.0); // Net funding rate should be positive (annualized)
    
    // With positive funding and perp premium, there should be a profitable opportunity
    EXPECT_GT(opp.estimated_profit, 0.0);
}

TEST_F(SameExchangeSpotPerpStrategyTest, NoOpportunityWithSmallFundingRate) {
    // Set up instruments
    setupBasicInstruments();
    
    // But set funding rate too low to be profitable
    mock_exchange_->setupFundingRate("BTC/USDT_PERP", 0.00001); // Only 0.001% funding
    
    // Find opportunities
    auto opportunities = strategy_->findOpportunities();
    
    // Should not find profitable opportunities with such low funding
    EXPECT_TRUE(opportunities.empty());
}

TEST_F(SameExchangeSpotPerpStrategyTest, ValidatesExistingOpportunity) {
    // Set up our test
    setupBasicInstruments();
    
    // Get an opportunity
    auto opportunities = strategy_->findOpportunities();
    ASSERT_FALSE(opportunities.empty());
    
    // Validate it with unchanged market conditions
    bool is_valid = strategy_->validateOpportunity(opportunities[0]);
    EXPECT_TRUE(is_valid);
    
    // Change funding rate dramatically and validate again
    mock_exchange_->setupFundingRate("BTC/USDT_PERP", -0.0005); // Flipped to negative
    is_valid = strategy_->validateOpportunity(opportunities[0]);
    EXPECT_FALSE(is_valid); // Should no longer be valid
}

TEST_F(SameExchangeSpotPerpStrategyTest, CalculatesPositionSizeBasedOnLiquidity) {
    // Set up our test
    setupBasicInstruments();
    
    // Get an opportunity
    auto opportunities = strategy_->findOpportunities();
    ASSERT_FALSE(opportunities.empty());
    
    // Calculate position size
    double size = strategy_->calculateOptimalPositionSize(opportunities[0]);
    
    // Should be positive and reasonable
    EXPECT_GT(size, 0.0);
    EXPECT_LT(size, 100000.0); // Sanity check - shouldn't be unreasonably large
}

} // namespace testing
} // namespace funding 