#include <gtest/gtest.h>
#include <strategy/composite_strategy.h>
#include <strategy/arbitrage_strategy.h>
#include <exchange/types.h>
#include "../exchange/mock_exchange.h"
#include <memory>

namespace funding {
namespace testing {

// A simple mock strategy for testing composition
class MockStrategy : public ArbitrageStrategy {
public:
    MockStrategy(const std::string& exchange1, const std::string& exchange2) 
        : exchange1_(exchange1), exchange2_(exchange2) {}
    
    // Mock opportunity finder that returns predefined opportunities
    std::vector<ArbitrageOpportunity> findOpportunities() override {
        std::vector<ArbitrageOpportunity> opportunities;
        
        if (!opportunities_.empty()) {
            return opportunities_;
        }
        
        // Create a default opportunity if none set
        ArbitrageOpportunity opp;
        opp.pair.exchange1 = exchange1_;
        opp.pair.symbol1 = "BTC/USDT";
        opp.pair.market_type1 = MarketType::PERPETUAL;
        opp.pair.exchange2 = exchange2_;
        opp.pair.symbol2 = "BTC/USDT";
        opp.pair.market_type2 = MarketType::PERPETUAL;
        opp.estimated_profit = 5.0;
        
        opportunities.push_back(opp);
        return opportunities;
    }
    
    bool validateOpportunity(const ArbitrageOpportunity& opportunity) override {
        // Is this opportunity managed by this strategy?
        return opportunity.pair.exchange1 == exchange1_ && 
               opportunity.pair.exchange2 == exchange2_;
    }
    
    double calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) override {
        return opportunity.estimated_profit * 10.0; // Just for testing
    }
    
    bool executeTrade(const ArbitrageOpportunity& opportunity, double size) override {
        last_executed_opportunity_ = opportunity;
        last_executed_size_ = size;
        return true;
    }
    
    bool closePosition(const ArbitrageOpportunity& opportunity) override {
        last_closed_opportunity_ = opportunity;
        return true;
    }
    
    void monitorPositions() override {
        monitor_called_ = true;
    }
    
    // Helper methods for testing
    void setOpportunities(const std::vector<ArbitrageOpportunity>& opportunities) {
        opportunities_ = opportunities;
    }
    
    bool wasMonitorCalled() const { return monitor_called_; }
    
    const ArbitrageOpportunity& getLastExecutedOpportunity() const { 
        return last_executed_opportunity_; 
    }
    
    double getLastExecutedSize() const { return last_executed_size_; }
    
    const ArbitrageOpportunity& getLastClosedOpportunity() const { 
        return last_closed_opportunity_; 
    }
    
private:
    std::string exchange1_;
    std::string exchange2_;
    std::vector<ArbitrageOpportunity> opportunities_;
    ArbitrageOpportunity last_executed_opportunity_;
    ArbitrageOpportunity last_closed_opportunity_;
    double last_executed_size_ = 0.0;
    bool monitor_called_ = false;
};

class CompositeStrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create multiple mock strategies
        auto strategy1 = std::make_unique<MockStrategy>("Binance", "Bybit");
        auto strategy2 = std::make_unique<MockStrategy>("OKX", "Deribit");
        
        // Store pointers for test access
        strategy1_ptr_ = strategy1.get();
        strategy2_ptr_ = strategy2.get();
        
        // Create vector of strategies
        std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
        strategies.push_back(std::move(strategy1));
        strategies.push_back(std::move(strategy2));
        
        // Create composite strategy
        composite_strategy_ = std::make_unique<CompositeStrategy>(std::move(strategies));
    }
    
    // Create opportunity for a given exchange pair
    ArbitrageOpportunity createOpportunity(
        const std::string& exchange1, 
        const std::string& exchange2,
        double profit,
        int strategy_index = -1) {
        ArbitrageOpportunity opp;
        opp.pair.exchange1 = exchange1;
        opp.pair.symbol1 = "BTC/USDT";
        opp.pair.market_type1 = MarketType::PERPETUAL;
        opp.pair.exchange2 = exchange2;
        opp.pair.symbol2 = "BTC/USDT";
        opp.pair.market_type2 = MarketType::PERPETUAL;
        opp.estimated_profit = profit;
        opp.strategy_type = "MockStrategy";
        opp.strategy_index = strategy_index;
        return opp;
    }
    
    std::unique_ptr<CompositeStrategy> composite_strategy_;
    MockStrategy* strategy1_ptr_; // Pointer to the first strategy within the composite
    MockStrategy* strategy2_ptr_; // Pointer to the second strategy within the composite
};

TEST_F(CompositeStrategyTest, CombinesOpportunitiesFromAllStrategies) {
    // Set up different opportunities for each strategy
    std::vector<ArbitrageOpportunity> opps1;
    opps1.push_back(createOpportunity("Binance", "Bybit", 10.0));
    opps1.push_back(createOpportunity("Binance", "Bybit", 5.0));
    strategy1_ptr_->setOpportunities(opps1);
    
    std::vector<ArbitrageOpportunity> opps2;
    opps2.push_back(createOpportunity("OKX", "Deribit", 15.0));
    opps2.push_back(createOpportunity("OKX", "Deribit", 8.0));
    strategy2_ptr_->setOpportunities(opps2);
    
    // Find combined opportunities
    auto opportunities = composite_strategy_->findOpportunities();
    
    // Should have all opportunities from both strategies
    ASSERT_EQ(4, opportunities.size());
    
    // Should be sorted by profit (highest first)
    EXPECT_EQ("OKX", opportunities[0].pair.exchange1);
    EXPECT_EQ("Deribit", opportunities[0].pair.exchange2);
    EXPECT_FLOAT_EQ(15.0, opportunities[0].estimated_profit);
    
    EXPECT_EQ("Binance", opportunities[1].pair.exchange1);
    EXPECT_EQ("Bybit", opportunities[1].pair.exchange2);
    EXPECT_FLOAT_EQ(10.0, opportunities[1].estimated_profit);
}

TEST_F(CompositeStrategyTest, DelegatesValidationToCorrectStrategy) {
    // Create an opportunity with strategy_index=0 (first strategy)
    auto binance_bybit_opp = createOpportunity("Binance", "Bybit", 10.0, 0);
    
    // Validate
    bool result = composite_strategy_->validateOpportunity(binance_bybit_opp);
    
    // Should be delegated to strategy1
    EXPECT_TRUE(result);
}

TEST_F(CompositeStrategyTest, DelegatesTradeExecutionToCorrectStrategy) {
    // Create an opportunity with strategy_index=0 (first strategy)
    auto binance_bybit_opp = createOpportunity("Binance", "Bybit", 10.0, 0);
    
    // Execute trade
    bool result = composite_strategy_->executeTrade(binance_bybit_opp, 1000.0);
    
    // Should be delegated to strategy1
    EXPECT_TRUE(result);
    EXPECT_EQ("Binance", strategy1_ptr_->getLastExecutedOpportunity().pair.exchange1);
    EXPECT_EQ("Bybit", strategy1_ptr_->getLastExecutedOpportunity().pair.exchange2);
    EXPECT_FLOAT_EQ(1000.0, strategy1_ptr_->getLastExecutedSize());
}

TEST_F(CompositeStrategyTest, DelegatesPositionCloseToCorrectStrategy) {
    // Create an opportunity with strategy_index=1 (second strategy)
    auto okx_deribit_opp = createOpportunity("OKX", "Deribit", 15.0, 1);
    
    // Close position
    bool result = composite_strategy_->closePosition(okx_deribit_opp);
    
    // Should be delegated to strategy2
    EXPECT_TRUE(result);
    EXPECT_EQ("OKX", strategy2_ptr_->getLastClosedOpportunity().pair.exchange1);
    EXPECT_EQ("Deribit", strategy2_ptr_->getLastClosedOpportunity().pair.exchange2);
}

TEST_F(CompositeStrategyTest, MonitorsAllStrategies) {
    // Call monitor positions
    composite_strategy_->monitorPositions();
    
    // Both strategies should have been called
    EXPECT_TRUE(strategy1_ptr_->wasMonitorCalled());
    EXPECT_TRUE(strategy2_ptr_->wasMonitorCalled());
}

} // namespace testing
} // namespace funding 