#pragma once

#include <vector>
#include <memory>
#include <string>
#include <exchange/types.h>
#include <exchange/exchange_interface.h>

namespace funding {

// Base class for all funding rate arbitrage strategies
class ArbitrageStrategy {
public:
    virtual ~ArbitrageStrategy() = default;
    
    // Find opportunities across all configured exchange pairs
    virtual std::vector<ArbitrageOpportunity> findOpportunities() = 0;
    
    // Evaluate if an opportunity is still valid
    virtual bool validateOpportunity(const ArbitrageOpportunity& opportunity) = 0;
    
    // Calculate the optimal position size for an opportunity
    virtual double calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) = 0;
    
    // Execute a trade for the given opportunity and size
    virtual bool executeTrade(const ArbitrageOpportunity& opportunity, double size) = 0;
    
    // Close an existing position
    virtual bool closePosition(const ArbitrageOpportunity& opportunity) = 0;
    
    // Monitor active positions
    virtual void monitorPositions() {}
    
    // Configuration methods
    virtual void setMinFundingRate(double rate) { min_funding_rate_ = rate; }
    virtual void setMinExpectedProfit(double profit) { min_expected_profit_ = profit; }
    virtual double getMinFundingRate() const { return min_funding_rate_; }
    virtual double getMinExpectedProfit() const { return min_expected_profit_; }

protected:
    double min_funding_rate_ = 0.0001;     // Minimum funding rate to consider (0.01%)
    double min_expected_profit_ = 1.0;     // Minimum expected profit (1%)
};

// For arbitrage between spot and perpetual markets on the same exchange
class SameExchangeSpotPerpStrategy : public ArbitrageStrategy {
public:
    explicit SameExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> exchange);
    
    std::vector<ArbitrageOpportunity> findOpportunities() override;
    bool validateOpportunity(const ArbitrageOpportunity& opportunity) override;
    double calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) override;
    bool executeTrade(const ArbitrageOpportunity& opportunity, double size) override;
    bool closePosition(const ArbitrageOpportunity& opportunity) override;
    void monitorPositions() override;

private:
    std::shared_ptr<ExchangeInterface> exchange_;
    
    /**
     * Calculate a risk score for the opportunity based on various factors.
     * @param opportunity The arbitrage opportunity to evaluate
     * @return A risk score from 0-100, where higher values indicate higher risk
     */
    double calculateRiskScore(const ArbitrageOpportunity& opportunity);
};

// For arbitrage between perpetual markets across different exchanges
class CrossExchangePerpStrategy : public ArbitrageStrategy {
public:
    CrossExchangePerpStrategy(std::shared_ptr<ExchangeInterface> exchange1,
                            std::shared_ptr<ExchangeInterface> exchange2);
    
    std::vector<ArbitrageOpportunity> findOpportunities() override;
    bool validateOpportunity(const ArbitrageOpportunity& opportunity) override;
    double calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) override;
    bool executeTrade(const ArbitrageOpportunity& opportunity, double size) override;
    bool closePosition(const ArbitrageOpportunity& opportunity) override;
    void monitorPositions() override;

private:
    // Calculate exchange risk based on historical reliability
    double calculateExchangeRisk(const std::string& exchange_name);
    
    // Check if we have sufficient margin for the execution
    bool checkMarginRequirements(const Order& order1, const Order& order2);
    
    // Wait for an order to be filled
    bool waitForOrderFill(std::shared_ptr<ExchangeInterface> exchange, 
                        const std::string& order_id, 
                        int max_attempts);
    
    // Verify that positions are fully closed
    bool verifyPositionClosed(const ArbitrageOpportunity& opportunity);
    
    std::shared_ptr<ExchangeInterface> exchange1_;
    std::shared_ptr<ExchangeInterface> exchange2_;
};

// Specialized strategy for spot on one exchange, perp on another
class CrossExchangeSpotPerpStrategy : public ArbitrageStrategy {
public:
    CrossExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> spot_exchange,
                                std::shared_ptr<ExchangeInterface> perp_exchange);
    
    std::vector<ArbitrageOpportunity> findOpportunities() override;
    bool validateOpportunity(const ArbitrageOpportunity& opportunity) override;
    double calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) override;
    bool executeTrade(const ArbitrageOpportunity& opportunity, double size) override;
    bool closePosition(const ArbitrageOpportunity& opportunity) override;
    void monitorPositions() override;

private:
    std::shared_ptr<ExchangeInterface> spot_exchange_;
    std::shared_ptr<ExchangeInterface> perp_exchange_;
};

// Factory function to create strategy based on configuration
std::unique_ptr<ArbitrageStrategy> createStrategy(
    const std::string& strategy_type,
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate = 0.0001,
    double min_expected_profit = 1.0);

} // namespace funding 