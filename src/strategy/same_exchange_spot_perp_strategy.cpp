#include <strategy/arbitrage_strategy.h>
#include <exchange/exchange_interface.h>
#include <exchange/types.h>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace funding {

SameExchangeSpotPerpStrategy::SameExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> exchange)
    : exchange_(exchange) {
}

std::vector<ArbitrageOpportunity> SameExchangeSpotPerpStrategy::findOpportunities() {
    std::vector<ArbitrageOpportunity> opportunities;
    
    try {
        // Get all available perpetual futures
        auto perp_instruments = exchange_->getAvailableInstruments(MarketType::PERPETUAL);
        auto spot_instruments = exchange_->getAvailableInstruments(MarketType::SPOT);
        
        // Find matching spot/perp pairs
        for (const auto& perp : perp_instruments) {
            // Find matching spot instrument
            auto spot_it = std::find_if(spot_instruments.begin(), spot_instruments.end(),
                [&perp](const Instrument& spot) {
                    return spot.base_currency == perp.base_currency && 
                           spot.quote_currency == perp.quote_currency;
                });
            
            if (spot_it == spot_instruments.end()) {
                continue; // No matching spot instrument found
            }
            
            // Get funding rate for the perpetual
            FundingRate funding = exchange_->getFundingRate(perp.symbol);
            
            // Skip if funding rate is too small to be profitable
            if (std::abs(funding.rate) < 0.0001) { // 0.01% threshold - configurable
                continue;
            }
            
            // Get prices and calculate spread
            double spot_price = exchange_->getPrice(spot_it->symbol);
            double perp_price = exchange_->getPrice(perp.symbol);
            double price_spread_pct = (perp_price - spot_price) / spot_price * 100.0;
            
            // Calculate annualized funding rate
            double hours_per_year = 24.0 * 365.0;
            double payments_per_year = hours_per_year / funding.payment_interval.count();
            double annualized_rate = funding.rate * payments_per_year * 100.0; // Convert to percentage
            
            // Calculate max allowable spread before it negates funding
            double max_spread = std::abs(annualized_rate) * 0.75; // 75% of funding rate
            
            // Get transaction fees for both spot and perp
            double spot_taker_fee = exchange_->getTradingFee(spot_it->symbol, false);
            double perp_taker_fee = exchange_->getTradingFee(perp.symbol, false);
            
            // Calculate total transaction cost for the complete arbitrage (entry + exit)
            double total_transaction_cost_pct = (spot_taker_fee + perp_taker_fee) * 2 * 100.0; // as percentage
            
            // Create trading pair
            TradingPair pair(exchange_->getName(), 
                           spot_it->symbol, 
                           MarketType::SPOT,
                           exchange_->getName(),
                           perp.symbol,
                           MarketType::PERPETUAL);
            
            ArbitrageOpportunity opportunity;
            opportunity.pair = pair;
            opportunity.funding_rate1 = 0.0; // Spot doesn't pay funding
            opportunity.funding_rate2 = funding.rate;
            opportunity.net_funding_rate = annualized_rate;
            opportunity.payment_interval1 = std::chrono::hours(0);
            opportunity.payment_interval2 = funding.payment_interval;
            opportunity.entry_price_spread = price_spread_pct;
            opportunity.max_allowable_spread = max_spread;
            opportunity.transaction_cost_pct = total_transaction_cost_pct;
            
            // Updated profit estimate including transaction costs
            opportunity.estimated_profit = std::abs(annualized_rate) - std::abs(price_spread_pct) - total_transaction_cost_pct;
            
            // Calculate the number of funding periods needed to break even on transaction costs
            double funding_per_period = std::abs(funding.rate) * 100.0; // As percentage
            opportunity.periods_to_breakeven = total_transaction_cost_pct / funding_per_period;
            
            // Get order book to estimate max position size
            OrderBook spot_ob = exchange_->getOrderBook(spot_it->symbol, 10);
            OrderBook perp_ob = exchange_->getOrderBook(perp.symbol, 10);
            
            double spot_liquidity = 0.0;
            double perp_liquidity = 0.0;
            
            // Calculate available liquidity (simplified)
            for (const auto& level : spot_ob.bids) {
                spot_liquidity += level.amount * level.price;
                if (spot_liquidity > 50000.0) break; // $50k USD limit for example
            }
            
            for (const auto& level : perp_ob.asks) {
                perp_liquidity += level.amount * level.price;
                if (perp_liquidity > 50000.0) break; // $50k USD limit for example
            }
            
            // Use smaller of the two for max position size
            opportunity.max_position_size = std::min(spot_liquidity, perp_liquidity) * 0.25; // 25% of available liquidity
            
            // Calculate a basic risk score (0-100), higher is riskier
            // Factors: spread vs funding rate, liquidity, funding volatility
            double spread_risk = std::abs(price_spread_pct / max_spread) * 50.0; // 50% of score
            double liquidity_risk = (1.0 - std::min(spot_liquidity, perp_liquidity) / 50000.0) * 30.0; // 30% of score
            double funding_risk = 20.0; // Base funding risk (would be better with historical data)
            
            opportunity.position_risk_score = std::min(100.0, spread_risk + liquidity_risk + funding_risk);
            
            // Record discovery time
            opportunity.discovery_time = std::chrono::system_clock::now();
            
            // Add to opportunities if estimated profit is positive AFTER transaction costs
            if (opportunity.estimated_profit > 0) {
                opportunities.push_back(opportunity);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in SameExchangeSpotPerpStrategy::findOpportunities: " 
                 << e.what() << std::endl;
    }
    
    // Sort opportunities by estimated profit (highest first)
    std::sort(opportunities.begin(), opportunities.end(),
             [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                 return a.estimated_profit > b.estimated_profit;
             });
    
    return opportunities;
}

bool SameExchangeSpotPerpStrategy::validateOpportunity(const ArbitrageOpportunity& opportunity) {
    try {
        // Get current funding rate
        FundingRate funding = exchange_->getFundingRate(opportunity.pair.symbol2);
        
        // Get current prices
        double spot_price = exchange_->getPrice(opportunity.pair.symbol1);
        double perp_price = exchange_->getPrice(opportunity.pair.symbol2);
        
        // Calculate current spread
        double current_spread_pct = (perp_price - spot_price) / spot_price * 100.0;
        
        // Check if spread has widened too much
        if (std::abs(current_spread_pct) > opportunity.max_allowable_spread) {
            return false;
        }
        
        // Check if funding rate has changed significantly
        double hours_per_year = 24.0 * 365.0;
        double payments_per_year = hours_per_year / funding.payment_interval.count();
        double current_annualized_rate = funding.rate * payments_per_year * 100.0;
        
        // If funding rate changed by more than 20%, invalidate opportunity
        if (std::abs(current_annualized_rate - opportunity.net_funding_rate) / 
            std::abs(opportunity.net_funding_rate) > 0.2) {
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in SameExchangeSpotPerpStrategy::validateOpportunity: " 
                 << e.what() << std::endl;
        return false;
    }
}

double SameExchangeSpotPerpStrategy::calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) {
    // This would normally incorporate risk factors, account size, etc.
    // For simplicity, we're returning a percentage of the max position size
    return opportunity.max_position_size * 0.5; // 50% of max
}

bool SameExchangeSpotPerpStrategy::executeTrade(const ArbitrageOpportunity& opportunity, double size) {
    try {
        // Determine direction based on funding rate sign
        // If funding rate is positive, go short perp and long spot
        // If funding rate is negative, go long perp and short spot
        bool is_funding_positive = opportunity.funding_rate2 > 0;
        
        // Order sides
        OrderSide spot_side = is_funding_positive ? OrderSide::BUY : OrderSide::SELL;
        OrderSide perp_side = is_funding_positive ? OrderSide::SELL : OrderSide::BUY;
        
        // Get current prices
        double spot_price = exchange_->getPrice(opportunity.pair.symbol1);
        double perp_price = exchange_->getPrice(opportunity.pair.symbol2);
        
        // Calculate position sizes in base currency
        double position_value = size; // USD value
        double spot_size = position_value / spot_price;
        double perp_size = position_value / perp_price;
        
        // Execute spot order first (less slippage usually)
        std::string spot_order_id = exchange_->placeOrder(
            opportunity.pair.symbol1,
            spot_side,
            OrderType::MARKET,
            spot_size
        );
        
        if (spot_order_id.empty()) {
            std::cerr << "Failed to place spot order" << std::endl;
            return false;
        }
        
        // Execute perp order
        std::string perp_order_id = exchange_->placeOrder(
            opportunity.pair.symbol2,
            perp_side,
            OrderType::MARKET,
            perp_size
        );
        
        if (perp_order_id.empty()) {
            std::cerr << "Failed to place perp order. Attempting to close spot position." << std::endl;
            
            // Try to close the spot position
            exchange_->placeOrder(
                opportunity.pair.symbol1,
                spot_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY,
                OrderType::MARKET,
                spot_size
            );
            
            return false;
        }
        
        std::cout << "Successfully executed arbitrage trade for " 
                 << opportunity.pair.symbol1 << " and " << opportunity.pair.symbol2 
                 << " with size " << size << " USD" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in SameExchangeSpotPerpStrategy::executeTrade: " 
                 << e.what() << std::endl;
        return false;
    }
}

bool SameExchangeSpotPerpStrategy::closePosition(const ArbitrageOpportunity& opportunity) {
    try {
        // Get current positions
        auto positions = exchange_->getPositions();
        
        // Find matching perpetual position
        auto perp_pos = std::find_if(positions.begin(), positions.end(),
                                    [&opportunity](const Position& pos) {
                                        return pos.symbol == opportunity.pair.symbol2;
                                    });
        
        if (perp_pos == positions.end()) {
            std::cerr << "No open perpetual position found for " 
                     << opportunity.pair.symbol2 << std::endl;
            return false;
        }
        
        // Determine closing orders
        OrderSide perp_close_side = perp_pos->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        double perp_size = std::abs(perp_pos->size);
        
        // Get balances to find spot position
        auto balances = exchange_->getBalances();
        std::string base_currency = opportunity.pair.symbol1.substr(0, opportunity.pair.symbol1.find('/'));
        
        if (balances.find(base_currency) == balances.end()) {
            std::cerr << "No " << base_currency << " balance found" << std::endl;
            return false;
        }
        
        double spot_size = balances[base_currency];
        OrderSide spot_close_side = perp_close_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
        
        // Close perp position
        std::string perp_order_id = exchange_->placeOrder(
            opportunity.pair.symbol2,
            perp_close_side,
            OrderType::MARKET,
            perp_size
        );
        
        if (perp_order_id.empty()) {
            std::cerr << "Failed to close perpetual position" << std::endl;
            return false;
        }
        
        // Close spot position
        std::string spot_order_id = exchange_->placeOrder(
            opportunity.pair.symbol1,
            spot_close_side,
            OrderType::MARKET,
            spot_size
        );
        
        if (spot_order_id.empty()) {
            std::cerr << "Failed to close spot position" << std::endl;
            return false;
        }
        
        std::cout << "Successfully closed arbitrage position for " 
                 << opportunity.pair.symbol1 << " and " << opportunity.pair.symbol2 << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in SameExchangeSpotPerpStrategy::closePosition: " 
                 << e.what() << std::endl;
        return false;
    }
}

void SameExchangeSpotPerpStrategy::monitorPositions() {
    try {
        // Get all positions on the exchange
        auto perp_positions = exchange_->getPositions();
        auto balances = exchange_->getBalances();
        
        // For each active position
        for (const auto& perp_pos : perp_positions) {
            // Skip small/dust positions
            if (std::abs(perp_pos.size) * perp_pos.entry_price < 10.0) { // $10 minimum
                continue;
            }
            
            // Extract the base currency from the symbol (e.g. "BTC" from "BTC/USDT")
            std::string symbol = perp_pos.symbol;
            std::string base_currency = symbol.substr(0, symbol.find('/'));
            
            // Skip if we don't have a corresponding spot position
            if (balances.find(base_currency) == balances.end() || 
                std::abs(balances[base_currency]) < 0.00001) {
                continue;
            }
            
            // Get current prices
            double spot_price = exchange_->getPrice(symbol);
            double perp_price = exchange_->getPrice(symbol);
            
            // Calculate current spread
            double current_spread_pct = (perp_price - spot_price) / spot_price * 100.0;
            double spot_size = balances[base_currency];
            
            // Get funding rate information
            FundingRate funding = exchange_->getFundingRate(symbol);
            
            // Calculate annualized funding rate
            double hours_per_year = 24.0 * 365.0;
            double payments_per_year = hours_per_year / funding.payment_interval.count();
            double annualized_rate = funding.rate * payments_per_year * 100.0;
            
            // Calculate max allowable spread
            double max_allowable_spread = std::abs(annualized_rate) / payments_per_year * 0.75;
            
            // Define spread warning thresholds for partial reduction (50%, 70%, 85% of max)
            double warning_threshold_1 = max_allowable_spread * 0.5;  // 50% of max - reduce by 20%
            double warning_threshold_2 = max_allowable_spread * 0.7;  // 70% of max - reduce by 30%
            double warning_threshold_3 = max_allowable_spread * 0.85; // 85% of max - reduce by 50%
            
            // Skip if the current spread is small (in safe zone)
            if (std::abs(current_spread_pct) < warning_threshold_1) {
                continue;
            }
            
            // Calculate position value
            double position_value = std::abs(perp_pos.size) * perp_price;
            
            // Determine the side of perp position (long or short)
            bool is_perp_long = perp_pos.size > 0;
            
            // Determine order sides for closing
            OrderSide perp_close_side = is_perp_long ? OrderSide::SELL : OrderSide::BUY;
            OrderSide spot_close_side = is_perp_long ? OrderSide::BUY : OrderSide::SELL;
            
            // Partial position reduction logic based on spread thresholds
            double reduction_pct = 0.0;
            
            if (std::abs(current_spread_pct) >= warning_threshold_3) {
                reduction_pct = 0.5; // Reduce by 50% at 85% of max spread
                std::cout << "Warning: Spread at 85% of maximum for " << symbol 
                          << ". Reducing position by 50%" << std::endl;
            } 
            else if (std::abs(current_spread_pct) >= warning_threshold_2) {
                reduction_pct = 0.3; // Reduce by 30% at 70% of max spread
                std::cout << "Warning: Spread at 70% of maximum for " << symbol 
                          << ". Reducing position by 30%" << std::endl;
            }
            else if (std::abs(current_spread_pct) >= warning_threshold_1) {
                reduction_pct = 0.2; // Reduce by 20% at 50% of max spread
                std::cout << "Warning: Spread at 50% of maximum for " << symbol 
                          << ". Reducing position by 20%" << std::endl;
            }
            
            // If we need to reduce position
            if (reduction_pct > 0.0) {
                double perp_reduce_size = std::abs(perp_pos.size) * reduction_pct;
                double spot_reduce_size = std::abs(spot_size) * reduction_pct;
                
                // Reduce perp position first
                std::string perp_order_id = exchange_->placeOrder(
                    symbol,
                    perp_close_side,
                    OrderType::MARKET,
                    perp_reduce_size
                );
                
                if (perp_order_id.empty()) {
                    std::cerr << "Failed to reduce perpetual position for " << symbol << std::endl;
                    continue;
                }
                
                // Then reduce spot position
                std::string spot_order_id = exchange_->placeOrder(
                    symbol,
                    spot_close_side,
                    OrderType::MARKET,
                    spot_reduce_size
                );
                
                if (spot_order_id.empty()) {
                    std::cerr << "Failed to reduce spot position for " << symbol << std::endl;
                    // Try to revert the perp reduction to maintain hedge
                    exchange_->placeOrder(
                        symbol,
                        perp_close_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY,
                        OrderType::MARKET,
                        perp_reduce_size
                    );
                    continue;
                }
                
                std::cout << "Successfully reduced position for " << symbol 
                          << " by " << (reduction_pct * 100.0) << "%. "
                          << "Current spread: " << current_spread_pct << "%, "
                          << "Max allowed: " << max_allowable_spread << "%" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in SameExchangeSpotPerpStrategy::monitorPositions: " 
                 << e.what() << std::endl;
    }
}

} // namespace funding