#include <gtest/gtest.h>
#include <risk/risk_manager.h>
#include <exchange/types.h>
#include <map>

namespace funding {
namespace testing {

class RiskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Set up a standard risk configuration
        risk_config_.max_position_size_usd = 5000.0;
        risk_config_.max_total_position_usd = 50000.0;
        risk_config_.max_position_per_exchange = 0.3;
        risk_config_.max_price_divergence_pct = 0.5;
        risk_config_.target_profit_pct = 1.0;
        risk_config_.stop_loss_pct = 1.0;
        risk_config_.dynamic_position_sizing = true;
        risk_config_.min_liquidity_depth = 50000.0;
        
        // Create risk manager with this config
        risk_manager_ = std::make_unique<RiskManager>(risk_config_);
    }
    
    // Helper to create a basic arbitrage opportunity
    ArbitrageOpportunity createBasicOpportunity() {
        ArbitrageOpportunity opp;
        opp.pair.exchange1 = "Binance";
        opp.pair.symbol1 = "BTC/USDT";
        opp.pair.market_type1 = MarketType::SPOT;
        opp.pair.exchange2 = "Binance";
        opp.pair.symbol2 = "BTC/USDT_PERP";
        opp.pair.market_type2 = MarketType::PERPETUAL;
        opp.funding_rate1 = 0.0;  // Spot doesn't pay funding
        opp.funding_rate2 = 0.001; // 0.1% funding rate
        opp.payment_interval1 = std::chrono::hours(0);
        opp.payment_interval2 = std::chrono::hours(8);
        opp.entry_price_spread = 0.1; // 0.1% price difference
        opp.max_allowable_spread = 0.3; // 0.3% max spread
        opp.transaction_cost_pct = 0.05; // 0.05% transaction cost
        opp.estimated_profit = 5.0; // 5% annualized return
        opp.max_position_size = 10000.0; // $10k max based on liquidity
        opp.position_risk_score = 30.0; // Low-medium risk
        opp.discovery_time = std::chrono::system_clock::now();
        return opp;
    }
    
    // Helper to create an active arbitrage position
    ArbitragePosition createActivePosition(const ArbitrageOpportunity& opp, double size) {
        ArbitragePosition pos;
        pos.opportunity = opp;
        pos.position_size = size;
        pos.entry_time = std::chrono::system_clock::now();
        pos.entry_price1 = 50000.0; // BTC price in USDT
        pos.entry_price2 = 50050.0; // BTC perp price
        pos.current_price1 = 50000.0;
        pos.current_price2 = 50050.0;
        pos.initial_spread = 0.1; // 0.1%
        pos.current_spread = 0.1; // 0.1%
        pos.funding_collected = 0.0;
        pos.unrealized_pnl = 0.0;
        pos.position_id = "test-position-1";
        pos.is_active = true;
        return pos;
    }
    
    RiskConfig risk_config_;
    std::unique_ptr<RiskManager> risk_manager_;
};

TEST_F(RiskManagerTest, AllowsPositionWithinRiskLimits) {
    auto opportunity = createBasicOpportunity();
    
    // Should allow this opportunity which is within risk limits
    bool can_enter = risk_manager_->canEnterPosition(opportunity);
    EXPECT_TRUE(can_enter);
}

TEST_F(RiskManagerTest, RejectsPositionWithHighRiskScore) {
    auto opportunity = createBasicOpportunity();
    opportunity.position_risk_score = 95.0; // Very high risk
    
    // Should reject this high-risk opportunity
    bool can_enter = risk_manager_->canEnterPosition(opportunity);
    EXPECT_FALSE(can_enter);
}

TEST_F(RiskManagerTest, CalculatesAppropriatePositionSize) {
    auto opportunity = createBasicOpportunity();
    
    // Calculate position size
    double size = risk_manager_->calculatePositionSize(opportunity);
    
    // Should be within limits
    EXPECT_GT(size, 0.0);
    EXPECT_LE(size, risk_config_.max_position_size_usd);
    EXPECT_LE(size, opportunity.max_position_size);
}

TEST_F(RiskManagerTest, TracksRegisteredPositions) {
    auto opportunity = createBasicOpportunity();
    auto position = createActivePosition(opportunity, 2000.0);
    
    // Register the position
    risk_manager_->registerPosition(position);
    
    // Should be able to retrieve it
    auto active_positions = risk_manager_->getActivePositions();
    ASSERT_EQ(1, active_positions.size());
    EXPECT_EQ(position.position_id, active_positions[0].position_id);
}

TEST_F(RiskManagerTest, DetectsWhenToClosePosition) {
    auto opportunity = createBasicOpportunity();
    auto position = createActivePosition(opportunity, 2000.0);
    
    // Initially, shouldn't need to close
    EXPECT_FALSE(risk_manager_->shouldClosePosition(position));
    
    // Position that has realized target profit
    position.unrealized_pnl = position.position_size * risk_config_.target_profit_pct / 100.0;
    EXPECT_TRUE(risk_manager_->shouldClosePosition(position));
    
    // Reset PnL
    position.unrealized_pnl = 0.0;
    
    // Position with spread exceeding max allowable
    position.current_spread = opportunity.max_allowable_spread * 1.5;
    EXPECT_TRUE(risk_manager_->shouldClosePosition(position));
}

TEST_F(RiskManagerTest, DetectsWhenToReducePosition) {
    auto opportunity = createBasicOpportunity();
    auto position = createActivePosition(opportunity, 2000.0);
    
    // Initially, shouldn't need to reduce
    double reduce_percent = 0.0;
    EXPECT_FALSE(risk_manager_->shouldReducePosition(position, reduce_percent));
    
    // Position with widening spread but not yet critical
    position.current_spread = opportunity.max_allowable_spread * 0.8;
    EXPECT_TRUE(risk_manager_->shouldReducePosition(position, reduce_percent));
    EXPECT_GT(reduce_percent, 0.0);
    EXPECT_LT(reduce_percent, 100.0);
}

TEST_F(RiskManagerTest, CalculatesExpectedReturn) {
    auto opportunity = createBasicOpportunity();
    
    // Calculate expected return
    double expected_return = risk_manager_->calculateExpectedReturn(opportunity);
    
    // Should be close to the estimated profit
    EXPECT_NEAR(opportunity.estimated_profit, expected_return, 0.5);
}

TEST_F(RiskManagerTest, CalculatesRiskRewardRatio) {
    auto opportunity = createBasicOpportunity();
    
    // Calculate risk/reward ratio
    double risk_reward = risk_manager_->calculateRiskRewardRatio(opportunity);
    
    // Should be positive and reasonable
    EXPECT_GT(risk_reward, 0.0);
}

} // namespace testing
} // namespace funding 