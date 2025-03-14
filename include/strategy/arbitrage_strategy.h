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
    
    // Execute the arbitrage trade
    virtual bool executeTrade(const ArbitrageOpportunity& opportunity, double size) = 0;
    
    // Close an existing arbitrage position
    virtual bool closePosition(const ArbitrageOpportunity& opportunity) = 0;
    
    // Monitor running arbitrage positions
    virtual void monitorPositions() = 0;
};

// Specialized strategy for spot/perp on the same exchange
class SameExchangeSpotPerpStrategy : public ArbitrageStrategy {
public:
    SameExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> exchange);
    
    std::vector<ArbitrageOpportunity> findOpportunities() override;
    bool validateOpportunity(const ArbitrageOpportunity& opportunity) override;
    double calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) override;
    bool executeTrade(const ArbitrageOpportunity& opportunity, double size) override;
    bool closePosition(const ArbitrageOpportunity& opportunity) override;
    void monitorPositions() override;

private:
    std::shared_ptr<ExchangeInterface> exchange_;
};

// Specialized strategy for perp/perp on different exchanges
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
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges);

} // namespace funding 