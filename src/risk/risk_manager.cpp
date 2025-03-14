#include <risk/risk_manager.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace funding {

RiskManager::RiskManager(const RiskConfig& config)
    : config_(config) {
}

bool RiskManager::canEnterPosition(const ArbitrageOpportunity& opportunity) {
    // Check if we have too many positions already
    double total_position_value = 0.0;
    for (const auto& [id, position] : active_positions_) {
        if (position.is_active) {
            total_position_value += position.position_size;
        }
    }
    
    // Check if adding this position would exceed our max total position
    if (total_position_value + opportunity.max_position_size > config_.max_total_position_usd) {
        return false;
    }
    
    // Check if the opportunity's risk score is acceptable
    if (opportunity.position_risk_score > 75.0) {  // Arbitrary threshold
        return false;
    }
    
    // Check if the opportunity's estimated profit is positive
    if (opportunity.estimated_profit <= 0.0) {
        return false;
    }
    
    return true;
}

double RiskManager::calculatePositionSize(const ArbitrageOpportunity& opportunity) {
    // Start with the maximum position size from the opportunity
    double size = std::min(opportunity.max_position_size, config_.max_position_size_usd);
    
    // Adjust based on risk score
    if (opportunity.position_risk_score > 50.0) {
        // Reduce position size for higher risk
        size *= (1.0 - (opportunity.position_risk_score - 50.0) / 100.0);
    }
    
    // Ensure we don't exceed our max total position
    double total_position_value = 0.0;
    for (const auto& [id, position] : active_positions_) {
        if (position.is_active) {
            total_position_value += position.position_size;
        }
    }
    
    double available_capital = config_.max_total_position_usd - total_position_value;
    size = std::min(size, available_capital);
    
    return size;
}

double RiskManager::calculatePriceRisk(const ArbitrageOpportunity& opportunity) {
    // Simple implementation - in a real system this would be more sophisticated
    return 50.0;  // Medium risk
}

double RiskManager::calculateLiquidityRisk(const ArbitrageOpportunity& opportunity) {
    // Simple implementation - in a real system this would be more sophisticated
    return 50.0;  // Medium risk
}

double RiskManager::calculateFundingRateVolatilityRisk(const ArbitrageOpportunity& opportunity) {
    // Simple implementation - in a real system this would be more sophisticated
    return 50.0;  // Medium risk
}

double RiskManager::calculateCorrelationRisk(const TradingPair& pair) {
    // Simple implementation - in a real system this would be more sophisticated
    return 50.0;  // Medium risk
}

void RiskManager::registerPosition(const ArbitragePosition& position) {
    active_positions_[position.position_id] = position;
}

bool RiskManager::shouldClosePosition(const ArbitragePosition& position) {
    // Check if we've reached our target profit
    if (position.unrealized_pnl > 0 && 
        position.unrealized_pnl / position.position_size * 100.0 >= config_.target_profit_pct) {
        return true;
    }
    
    // Check if we've hit our stop loss
    if (position.unrealized_pnl < 0 && 
        std::abs(position.unrealized_pnl) / position.position_size * 100.0 >= config_.stop_loss_pct) {
        return true;
    }
    
    // Check if the price spread has diverged too much
    double current_spread_pct = std::abs(position.current_spread - position.initial_spread) / 
                               position.initial_spread * 100.0;
    if (current_spread_pct > config_.max_price_divergence_pct) {
        return true;
    }
    
    return false;
}

bool RiskManager::shouldReducePosition(const ArbitragePosition& position, double& reduce_percent) {
    // Default reduction percentage
    reduce_percent = 0.5;  // 50% reduction
    
    // Check if the price spread is approaching our divergence limit
    double current_spread_pct = std::abs(position.current_spread - position.initial_spread) / 
                               position.initial_spread * 100.0;
    
    // If we're at 75% of our max divergence, reduce position
    if (current_spread_pct > config_.max_price_divergence_pct * 0.75 &&
        current_spread_pct <= config_.max_price_divergence_pct) {
        return true;
    }
    
    // Check if we're approaching our stop loss
    double loss_pct = 0.0;
    if (position.unrealized_pnl < 0) {
        loss_pct = std::abs(position.unrealized_pnl) / position.position_size * 100.0;
        
        // If we're at 75% of our stop loss, reduce position
        if (loss_pct > config_.stop_loss_pct * 0.75 &&
            loss_pct <= config_.stop_loss_pct) {
            return true;
        }
    }
    
    return false;
}

double RiskManager::calculateExpectedReturn(const ArbitrageOpportunity& opportunity) {
    // Simple implementation - in a real system this would be more sophisticated
    return opportunity.estimated_profit;
}

double RiskManager::calculateMaxDrawdown(const ArbitrageOpportunity& opportunity) {
    // Simple implementation - in a real system this would be more sophisticated
    return opportunity.transaction_cost_pct;
}

double RiskManager::calculateRiskRewardRatio(const ArbitrageOpportunity& opportunity) {
    double expected_return = calculateExpectedReturn(opportunity);
    double max_drawdown = calculateMaxDrawdown(opportunity);
    
    if (max_drawdown <= 0.0) {
        return 0.0;  // Avoid division by zero
    }
    
    return expected_return / max_drawdown;
}

void RiskManager::updatePositionStatus(
    ArbitragePosition& position,
    const std::map<std::string, std::shared_ptr<ExchangeInterface>>& exchanges) {
    
    try {
        // Get current prices from exchanges
        auto exchange1_it = exchanges.find(position.opportunity.pair.exchange1);
        auto exchange2_it = exchanges.find(position.opportunity.pair.exchange2);
        
        if (exchange1_it != exchanges.end() && exchange2_it != exchanges.end()) {
            // Update current prices
            position.current_price1 = exchange1_it->second->getPrice(position.opportunity.pair.symbol1);
            position.current_price2 = exchange2_it->second->getPrice(position.opportunity.pair.symbol2);
            
            // Calculate current spread
            position.current_spread = std::abs(position.current_price1 - position.current_price2) / 
                                     ((position.current_price1 + position.current_price2) / 2.0) * 100.0;
            
            // Calculate unrealized PnL
            // This is a simplified calculation - in a real system this would be more sophisticated
            double price_pnl = (position.current_spread - position.initial_spread) * position.position_size;
            
            // Add funding collected
            position.unrealized_pnl = price_pnl + position.funding_collected;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error updating position status: " << e.what() << std::endl;
    }
}

std::vector<ArbitragePosition> RiskManager::getActivePositions() const {
    std::vector<ArbitragePosition> positions;
    
    for (const auto& [id, position] : active_positions_) {
        if (position.is_active) {
            positions.push_back(position);
        }
    }
    
    return positions;
}

double RiskManager::normalizeRate(double rate, const std::chrono::hours& interval) {
    // Convert to annual rate
    double hours_per_year = 24.0 * 365.0;
    double intervals_per_year = hours_per_year / interval.count();
    
    return rate * intervals_per_year;
}

double RiskManager::calculateMaxAllowableSpread(const ArbitrageOpportunity& opportunity) {
    // Calculate the maximum price spread that would still be profitable
    // This is a simplified calculation - in a real system this would be more sophisticated
    return opportunity.transaction_cost_pct + opportunity.net_funding_rate * 0.1;  // 10% of annual funding rate
}

std::string RiskManager::generatePositionId(const ArbitrageOpportunity& opportunity) {
    std::stringstream ss;
    ss << opportunity.pair.exchange1 << "_" 
       << opportunity.pair.symbol1 << "_"
       << opportunity.pair.exchange2 << "_"
       << opportunity.pair.symbol2 << "_"
       << std::chrono::duration_cast<std::chrono::milliseconds>(
           opportunity.discovery_time.time_since_epoch()).count();
    return ss.str();
}

} // namespace funding 