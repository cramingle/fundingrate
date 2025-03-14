#pragma once

#include <strategy/arbitrage_strategy.h>
#include <vector>
#include <memory>
#include <algorithm>

namespace funding {

// A strategy that combines multiple strategies into one
class CompositeStrategy : public ArbitrageStrategy {
public:
    CompositeStrategy(std::vector<std::unique_ptr<ArbitrageStrategy>> strategies)
        : strategies_(std::move(strategies)) {}
    
    std::vector<ArbitrageOpportunity> findOpportunities() override {
        std::vector<ArbitrageOpportunity> all_opportunities;
        
        // Collect opportunities from all sub-strategies
        for (const auto& strategy : strategies_) {
            auto opportunities = strategy->findOpportunities();
            all_opportunities.insert(all_opportunities.end(), 
                                    opportunities.begin(), 
                                    opportunities.end());
        }
        
        // Sort all opportunities by estimated profit (highest first)
        std::sort(all_opportunities.begin(), all_opportunities.end(),
                 [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                     return a.estimated_profit > b.estimated_profit;
                 });
        
        return all_opportunities;
    }
    
    bool validateOpportunity(const ArbitrageOpportunity& opportunity) override {
        // Find the strategy that handles this opportunity's exchange pair
        for (const auto& strategy : strategies_) {
            // This is a simplified approach - in a real implementation, 
            // we'd need a way to identify which strategy created an opportunity
            auto opportunities = strategy->findOpportunities();
            
            // Check if this strategy handles the exchange pair
            auto it = std::find_if(opportunities.begin(), opportunities.end(),
                [&opportunity](const ArbitrageOpportunity& opp) {
                    return opp.pair.exchange1 == opportunity.pair.exchange1 &&
                           opp.pair.symbol1 == opportunity.pair.symbol1 &&
                           opp.pair.exchange2 == opportunity.pair.exchange2 &&
                           opp.pair.symbol2 == opportunity.pair.symbol2;
                });
                
            if (it != opportunities.end()) {
                return strategy->validateOpportunity(opportunity);
            }
        }
        
        return false;
    }
    
    double calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) override {
        // Find the strategy that handles this opportunity's exchange pair
        for (const auto& strategy : strategies_) {
            auto opportunities = strategy->findOpportunities();
            
            auto it = std::find_if(opportunities.begin(), opportunities.end(),
                [&opportunity](const ArbitrageOpportunity& opp) {
                    return opp.pair.exchange1 == opportunity.pair.exchange1 &&
                           opp.pair.symbol1 == opportunity.pair.symbol1 &&
                           opp.pair.exchange2 == opportunity.pair.exchange2 &&
                           opp.pair.symbol2 == opportunity.pair.symbol2;
                });
                
            if (it != opportunities.end()) {
                return strategy->calculateOptimalPositionSize(opportunity);
            }
        }
        
        return 0.0;
    }
    
    bool executeTrade(const ArbitrageOpportunity& opportunity, double size) override {
        // Find the strategy that handles this opportunity's exchange pair
        for (const auto& strategy : strategies_) {
            auto opportunities = strategy->findOpportunities();
            
            auto it = std::find_if(opportunities.begin(), opportunities.end(),
                [&opportunity](const ArbitrageOpportunity& opp) {
                    return opp.pair.exchange1 == opportunity.pair.exchange1 &&
                           opp.pair.symbol1 == opportunity.pair.symbol1 &&
                           opp.pair.exchange2 == opportunity.pair.exchange2 &&
                           opp.pair.symbol2 == opportunity.pair.symbol2;
                });
                
            if (it != opportunities.end()) {
                return strategy->executeTrade(opportunity, size);
            }
        }
        
        return false;
    }
    
    bool closePosition(const ArbitrageOpportunity& opportunity) override {
        // Find the strategy that handles this opportunity's exchange pair
        for (const auto& strategy : strategies_) {
            auto opportunities = strategy->findOpportunities();
            
            auto it = std::find_if(opportunities.begin(), opportunities.end(),
                [&opportunity](const ArbitrageOpportunity& opp) {
                    return opp.pair.exchange1 == opportunity.pair.exchange1 &&
                           opp.pair.symbol1 == opportunity.pair.symbol1 &&
                           opp.pair.exchange2 == opportunity.pair.exchange2 &&
                           opp.pair.symbol2 == opportunity.pair.symbol2;
                });
                
            if (it != opportunities.end()) {
                return strategy->closePosition(opportunity);
            }
        }
        
        return false;
    }
    
    void monitorPositions() override {
        for (const auto& strategy : strategies_) {
            strategy->monitorPositions();
        }
    }
    
private:
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies_;
};

} // namespace funding 