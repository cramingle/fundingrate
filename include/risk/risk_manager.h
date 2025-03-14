#pragma once

#include <map>
#include <string>
#include <memory>
#include <exchange/types.h>
#include <exchange/exchange_interface.h>

namespace funding {

// Configuration for risk management
struct RiskConfig {
    double max_position_size_usd;        // Maximum position size in USD per opportunity
    double max_total_position_usd;       // Maximum total USD exposure across all positions
    double max_position_per_exchange;    // Maximum % of exchange balance to use
    double max_price_divergence_pct;     // Maximum allowed price divergence before reducing/closing
    double target_profit_pct;            // Target profit percentage to close positions
    double stop_loss_pct;                // Stop loss percentage
    bool dynamic_position_sizing;        // Whether to use dynamic sizing based on volatility
    double min_liquidity_depth;          // Minimum order book depth required for entry
};

// Tracks an active arbitrage position
struct ArbitragePosition {
    ArbitrageOpportunity opportunity;
    double position_size;               // Size of position
    std::chrono::system_clock::time_point entry_time;
    double entry_price1;
    double entry_price2;
    double current_price1;
    double current_price2;
    double current_spread;
    double initial_spread;
    double funding_collected;           // Total funding collected so far
    double unrealized_pnl;              // Current PnL
    std::string position_id;            // Unique identifier
    bool is_active;                     // Whether position is still active
};

// Manages risk for all arbitrage positions
class RiskManager {
public:
    RiskManager(const RiskConfig& config);
    
    // Position evaluation and sizing
    bool canEnterPosition(const ArbitrageOpportunity& opportunity);
    double calculatePositionSize(const ArbitrageOpportunity& opportunity);
    
    // Risk assessment
    double calculatePriceRisk(const ArbitrageOpportunity& opportunity);
    double calculateLiquidityRisk(const ArbitrageOpportunity& opportunity);
    double calculateFundingRateVolatilityRisk(const ArbitrageOpportunity& opportunity);
    double calculateCorrelationRisk(const TradingPair& pair);
    
    // Position management
    void registerPosition(const ArbitragePosition& position);
    bool shouldClosePosition(const ArbitragePosition& position);
    bool shouldReducePosition(const ArbitragePosition& position, double& reduce_percent);
    
    // Expected PnL and risk/reward
    double calculateExpectedReturn(const ArbitrageOpportunity& opportunity);
    double calculateMaxDrawdown(const ArbitrageOpportunity& opportunity);
    double calculateRiskRewardRatio(const ArbitrageOpportunity& opportunity);
    
    // Updates the position status with latest market data
    void updatePositionStatus(ArbitragePosition& position,
                             const std::map<std::string, std::shared_ptr<ExchangeInterface>>& exchanges);
    
    // Get all active positions
    std::vector<ArbitragePosition> getActivePositions() const;
    
private:
    RiskConfig config_;
    std::map<std::string, ArbitragePosition> active_positions_;
    
    // Utility methods
    double normalizeRate(double rate, const std::chrono::hours& interval);
    double calculateMaxAllowableSpread(const ArbitrageOpportunity& opportunity);
    std::string generatePositionId(const ArbitrageOpportunity& opportunity);
};

} // namespace funding 