#include <strategy/arbitrage_strategy.h>
#include <exchange/exchange_interface.h>
#include <exchange/types.h>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace funding {

CrossExchangeSpotPerpStrategy::CrossExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> spot_exchange,
                                                           std::shared_ptr<ExchangeInterface> perp_exchange)
    : spot_exchange_(spot_exchange), perp_exchange_(perp_exchange) {
}

std::vector<ArbitrageOpportunity> CrossExchangeSpotPerpStrategy::findOpportunities() {
    std::vector<ArbitrageOpportunity> opportunities;
    
    try {
        // Get all available instruments from both exchanges
        auto perp_instruments = perp_exchange_->getAvailableInstruments(MarketType::PERPETUAL);
        auto spot_instruments = spot_exchange_->getAvailableInstruments(MarketType::SPOT);
        
        // Find matching symbols across exchanges
        for (const auto& perp : perp_instruments) {
            // Find matching spot instrument on spot exchange
            auto spot_it = std::find_if(spot_instruments.begin(), spot_instruments.end(),
                [&perp](const Instrument& spot) {
                    return spot.base_currency == perp.base_currency && 
                           spot.quote_currency == perp.quote_currency;
                });
                
            if (spot_it == spot_instruments.end()) {
                continue; // No matching instrument found
            }
            
            // Get funding rate for perpetual
            FundingRate funding = perp_exchange_->getFundingRate(perp.symbol);
            
            // Skip if funding rate is too low (configurable threshold)
            if (std::abs(funding.rate) < 0.0001) { // 0.01% threshold
                continue;
            }
            
            // Get current prices
            double spot_price = spot_exchange_->getPrice(spot_it->symbol);
            double perp_price = perp_exchange_->getPrice(perp.symbol);
            
            // Calculate price spread
            double price_spread_pct = (perp_price - spot_price) / spot_price * 100.0;
            
            // Create trading pair
            TradingPair pair(spot_exchange_->getName(), 
                          spot_it->symbol, 
                          MarketType::SPOT,
                          perp_exchange_->getName(),
                          perp.symbol,
                          MarketType::PERPETUAL);
            
            // Calculate annualized funding rate
            double hours_per_year = 24.0 * 365.0;
            double payments_per_year = hours_per_year / funding.payment_interval.count();
            double annualized_rate = funding.rate * payments_per_year * 100.0;
            
            // Get transaction fees for both exchanges
            double spot_taker_fee = spot_exchange_->getTradingFee(spot_it->symbol, false);
            double perp_taker_fee = perp_exchange_->getTradingFee(perp.symbol, false);
            
            // Calculate total transaction cost for the complete arbitrage (entry + exit)
            // For each exchange: entry fee + exit fee (as percentage)
            double total_transaction_cost_pct = (spot_taker_fee + perp_taker_fee) * 2 * 100.0;
            
            // Calculate max allowable spread before it negates funding
            double max_spread = std::abs(annualized_rate) * 0.6; // 60% buffer for cross-exchange
            
            // Build opportunity
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
            
            // Get order books to estimate max position size
            OrderBook spot_ob = spot_exchange_->getOrderBook(spot_it->symbol, 10);
            OrderBook perp_ob = perp_exchange_->getOrderBook(perp.symbol, 10);
            
            double spot_liquidity = 0.0;
            double perp_liquidity = 0.0;
            
            // Calculate available liquidity (simplified)
            // Use bids or asks depending on which side we'd take
            bool long_spot = funding.rate > 0;
            
            if (long_spot) {
                // Long spot, short perp
                // Need ask liquidity on spot exchange, bid liquidity on perp exchange
                for (const auto& level : spot_ob.asks) {
                    spot_liquidity += level.amount * level.price;
                    if (spot_liquidity > 50000.0) break; // $50k USD limit
                }
                
                for (const auto& level : perp_ob.bids) {
                    perp_liquidity += level.amount * level.price;
                    if (perp_liquidity > 50000.0) break; // $50k USD limit
                }
            } else {
                // Short spot, long perp
                // Need bid liquidity on spot exchange, ask liquidity on perp exchange
                for (const auto& level : spot_ob.bids) {
                    spot_liquidity += level.amount * level.price;
                    if (spot_liquidity > 50000.0) break; // $50k USD limit
                }
                
                for (const auto& level : perp_ob.asks) {
                    perp_liquidity += level.amount * level.price;
                    if (perp_liquidity > 50000.0) break; // $50k USD limit
                }
            }
            
            // Use smaller of the two for max position size, with more conservative sizing for cross-exchange
            opportunity.max_position_size = std::min(spot_liquidity, perp_liquidity) * 0.2; // 20% of available liquidity
            
            // Calculate a basic risk score (0-100), higher is riskier
            double spread_risk = std::abs(price_spread_pct / max_spread) * 40.0; // 40% of score
            double liquidity_risk = (1.0 - std::min(spot_liquidity, perp_liquidity) / 50000.0) * 25.0; // 25% of score
            double exchange_risk = 15.0; // Higher exchange counterparty risk for cross-exchange
            double funding_risk = 20.0; // Base funding risk (would be better with historical data)
            
            opportunity.position_risk_score = std::min(100.0, spread_risk + liquidity_risk + exchange_risk + funding_risk);
            
            // Record discovery time
            opportunity.discovery_time = std::chrono::system_clock::now();
            
            // Add to opportunities if estimated profit is positive AFTER transaction costs
            if (opportunity.estimated_profit > 0) {
                opportunities.push_back(opportunity);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangeSpotPerpStrategy::findOpportunities: " 
                 << e.what() << std::endl;
    }
    
    // Sort opportunities by estimated profit (highest first)
    std::sort(opportunities.begin(), opportunities.end(),
             [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                 return a.estimated_profit > b.estimated_profit;
             });
    
    return opportunities;
}

bool CrossExchangeSpotPerpStrategy::validateOpportunity(const ArbitrageOpportunity& opportunity) {
    try {
        // Get current funding rate
        FundingRate funding = perp_exchange_->getFundingRate(opportunity.pair.symbol2);
        
        // Get current prices
        double spot_price = spot_exchange_->getPrice(opportunity.pair.symbol1);
        double perp_price = perp_exchange_->getPrice(opportunity.pair.symbol2);
        
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
        
        // If funding rate changed by more than 20% or flipped sign, invalidate opportunity
        if (std::abs(current_annualized_rate - opportunity.net_funding_rate) / 
            std::abs(opportunity.net_funding_rate) > 0.2 || 
            (current_annualized_rate * opportunity.net_funding_rate < 0)) {
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangeSpotPerpStrategy::validateOpportunity: " 
                 << e.what() << std::endl;
        return false;
    }
}

double CrossExchangeSpotPerpStrategy::calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) {
    // For cross-exchange spot-perp, be even more conservative with position sizing
    return opportunity.max_position_size * 0.35; // 35% of max position size
}

bool CrossExchangeSpotPerpStrategy::executeTrade(const ArbitrageOpportunity& opportunity, double size) {
    try {
        // Determine direction based on funding rate sign
        // If funding rate is positive, go long spot and short perp
        // If funding rate is negative, go short spot and long perp
        bool is_funding_positive = opportunity.funding_rate2 > 0;
        
        // Order sides
        OrderSide spot_side = is_funding_positive ? OrderSide::BUY : OrderSide::SELL;
        OrderSide perp_side = is_funding_positive ? OrderSide::SELL : OrderSide::BUY;
        
        // Get current prices
        double spot_price = spot_exchange_->getPrice(opportunity.pair.symbol1);
        double perp_price = perp_exchange_->getPrice(opportunity.pair.symbol2);
        
        // Calculate position sizes in base currency
        double position_value = size; // USD value
        double spot_size = position_value / spot_price;
        double perp_size = position_value / perp_price;
        
        // For cross-exchange, decide which to execute first based on available liquidity
        OrderBook spot_ob = spot_exchange_->getOrderBook(opportunity.pair.symbol1, 3);
        OrderBook perp_ob = perp_exchange_->getOrderBook(opportunity.pair.symbol2, 3);
        
        double spot_liquidity = 0.0;
        double perp_liquidity = 0.0;
        
        // Simplified liquidity estimation
        if (is_funding_positive) {
            // Long spot, need ask liquidity
            for (const auto& level : spot_ob.asks) spot_liquidity += level.amount;
            // Short perp, need bid liquidity
            for (const auto& level : perp_ob.bids) perp_liquidity += level.amount;
        } else {
            // Short spot, need bid liquidity
            for (const auto& level : spot_ob.bids) spot_liquidity += level.amount;
            // Long perp, need ask liquidity
            for (const auto& level : perp_ob.asks) perp_liquidity += level.amount;
        }
        
        // Execute more liquid market first
        bool execute_spot_first = spot_liquidity >= perp_liquidity;
        std::string spot_order_id;
        std::string perp_order_id;
        
        if (execute_spot_first) {
            // Execute spot order first
            spot_order_id = spot_exchange_->placeOrder(
                opportunity.pair.symbol1,
                spot_side,
                OrderType::MARKET,
                spot_size
            );
            
            if (spot_order_id.empty()) {
                std::cerr << "Failed to place spot order on " 
                         << spot_exchange_->getName() << std::endl;
                return false;
            }
            
            // Execute perp order
            perp_order_id = perp_exchange_->placeOrder(
                opportunity.pair.symbol2,
                perp_side,
                OrderType::MARKET,
                perp_size
            );
            
            if (perp_order_id.empty()) {
                std::cerr << "Failed to place perp order on " << perp_exchange_->getName() 
                         << ". Attempting to close spot position." << std::endl;
                
                // Try to close the spot position
                spot_exchange_->placeOrder(
                    opportunity.pair.symbol1,
                    spot_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY,
                    OrderType::MARKET,
                    spot_size
                );
                
                return false;
            }
        } else {
            // Execute perp order first
            perp_order_id = perp_exchange_->placeOrder(
                opportunity.pair.symbol2,
                perp_side,
                OrderType::MARKET,
                perp_size
            );
            
            if (perp_order_id.empty()) {
                std::cerr << "Failed to place perp order on " 
                         << perp_exchange_->getName() << std::endl;
                return false;
            }
            
            // Execute spot order
            spot_order_id = spot_exchange_->placeOrder(
                opportunity.pair.symbol1,
                spot_side,
                OrderType::MARKET,
                spot_size
            );
            
            if (spot_order_id.empty()) {
                std::cerr << "Failed to place spot order on " << spot_exchange_->getName() 
                         << ". Attempting to close perp position." << std::endl;
                
                // Try to close the perp position
                perp_exchange_->placeOrder(
                    opportunity.pair.symbol2,
                    perp_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY,
                    OrderType::MARKET,
                    perp_size
                );
                
                return false;
            }
        }
        
        std::cout << "Successfully executed cross-exchange spot-perp arbitrage trade for " 
                 << opportunity.pair.symbol1 << " on " << spot_exchange_->getName()
                 << " and " << opportunity.pair.symbol2 << " on " << perp_exchange_->getName()
                 << " with size " << size << " USD" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangeSpotPerpStrategy::executeTrade: " 
                 << e.what() << std::endl;
        return false;
    }
}

bool CrossExchangeSpotPerpStrategy::closePosition(const ArbitrageOpportunity& opportunity) {
    try {
        // Get perp positions
        auto perp_positions = perp_exchange_->getPositions();
        
        // Find matching perpetual position
        auto perp_pos = std::find_if(perp_positions.begin(), perp_positions.end(),
                                    [&opportunity](const Position& pos) {
                                        return pos.symbol == opportunity.pair.symbol2;
                                    });
        
        if (perp_pos == perp_positions.end()) {
            std::cerr << "No open perpetual position found for " 
                     << opportunity.pair.symbol2 << std::endl;
            return false;
        }
        
        // Determine closing orders
        OrderSide perp_close_side = perp_pos->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        double perp_size = std::abs(perp_pos->size);
        
        // Get spot balances to find spot position
        auto balances = spot_exchange_->getBalances();
        std::string base_currency = opportunity.pair.symbol1.substr(0, opportunity.pair.symbol1.find('/'));
        
        if (balances.find(base_currency) == balances.end() || std::abs(balances[base_currency]) < 0.00001) {
            std::cerr << "No " << base_currency << " balance found on spot exchange" << std::endl;
            return false;
        }
        
        double spot_size = balances[base_currency];
        OrderSide spot_close_side = perp_close_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
        
        // Decide which to close first based on current liquidity
        OrderBook spot_ob = spot_exchange_->getOrderBook(opportunity.pair.symbol1, 3);
        OrderBook perp_ob = perp_exchange_->getOrderBook(opportunity.pair.symbol2, 3);
        
        double spot_liquidity = 0.0;
        double perp_liquidity = 0.0;
        
        // Calculate liquidity for closing direction
        if (spot_close_side == OrderSide::SELL) {
            for (const auto& level : spot_ob.bids) spot_liquidity += level.amount;
        } else {
            for (const auto& level : spot_ob.asks) spot_liquidity += level.amount;
        }
        
        if (perp_close_side == OrderSide::SELL) {
            for (const auto& level : perp_ob.bids) perp_liquidity += level.amount;
        } else {
            for (const auto& level : perp_ob.asks) perp_liquidity += level.amount;
        }
        
        // Close less liquid position first
        bool close_spot_first = spot_liquidity <= perp_liquidity;
        
        if (close_spot_first) {
            // Close spot position first
            std::string spot_order_id = spot_exchange_->placeOrder(
                opportunity.pair.symbol1,
                spot_close_side,
                OrderType::MARKET,
                spot_size
            );
            
            if (spot_order_id.empty()) {
                std::cerr << "Failed to close spot position on " << spot_exchange_->getName() << std::endl;
                return false;
            }
            
            // Then close perp position
            std::string perp_order_id = perp_exchange_->placeOrder(
                opportunity.pair.symbol2,
                perp_close_side,
                OrderType::MARKET,
                perp_size
            );
            
            if (perp_order_id.empty()) {
                std::cerr << "Failed to close perpetual position on " << perp_exchange_->getName() << std::endl;
                return false;
            }
        } else {
            // Close perp position first
            std::string perp_order_id = perp_exchange_->placeOrder(
                opportunity.pair.symbol2,
                perp_close_side,
                OrderType::MARKET,
                perp_size
            );
            
            if (perp_order_id.empty()) {
                std::cerr << "Failed to close perpetual position on " << perp_exchange_->getName() << std::endl;
                return false;
            }
            
            // Then close spot position
            std::string spot_order_id = spot_exchange_->placeOrder(
                opportunity.pair.symbol1,
                spot_close_side,
                OrderType::MARKET,
                spot_size
            );
            
            if (spot_order_id.empty()) {
                std::cerr << "Failed to close spot position on " << spot_exchange_->getName() << std::endl;
                return false;
            }
        }
        
        std::cout << "Successfully closed cross-exchange spot-perp arbitrage position for " 
                 << opportunity.pair.symbol1 << " on " << spot_exchange_->getName()
                 << " and " << opportunity.pair.symbol2 << " on " << perp_exchange_->getName() << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangeSpotPerpStrategy::closePosition: " 
                 << e.what() << std::endl;
        return false;
    }
}

void CrossExchangeSpotPerpStrategy::monitorPositions() {
    try {
        // Get perp positions
        auto perp_positions = perp_exchange_->getPositions();
        
        // Get spot balances
        auto spot_balances = spot_exchange_->getBalances();
        
        // For each perp position, find corresponding spot position
        for (const auto& perp_pos : perp_positions) {
            // Skip small positions
            if (std::abs(perp_pos.size) * perp_pos.entry_price < 10.0) { // $10 minimum
                continue;
            }
            
            std::string symbol = perp_pos.symbol;
            
            // Extract base currency
            std::string base_currency = symbol.substr(0, symbol.find('/'));
            
            // Check if we have a corresponding spot balance
            if (spot_balances.find(base_currency) == spot_balances.end() || 
                std::abs(spot_balances[base_currency]) < 0.00001) {
                continue; // No matching spot position
            }
            
            double spot_size = spot_balances[base_currency];
            
            // Verify spot and perp positions have opposite signs (proper hedge)
            bool is_perp_long = perp_pos.size > 0;
            bool is_spot_long = spot_size > 0;
            
            if ((is_perp_long && is_spot_long) || (!is_perp_long && !is_spot_long)) {
                std::cerr << "Warning: Positions on " << symbol << " are not properly hedged" << std::endl;
                continue;
            }
            
            // Get current prices
            std::string spot_symbol = symbol; // Assumes same symbol format on both exchanges
            double spot_price = spot_exchange_->getPrice(spot_symbol);
            double perp_price = perp_exchange_->getPrice(symbol);
            
            // Calculate current spread
            double current_spread_pct = (perp_price - spot_price) / spot_price * 100.0;
            
            // Get funding rate
            FundingRate funding = perp_exchange_->getFundingRate(symbol);
            
            // Calculate annualized funding rate
            double hours_per_year = 24.0 * 365.0;
            double payments_per_year = hours_per_year / funding.payment_interval.count();
            double annualized_rate = funding.rate * payments_per_year * 100.0;
            
            // Calculate max allowable spread (70% of funding rate as buffer for cross-exchange)
            double max_allowable_spread = std::abs(annualized_rate) / payments_per_year * 0.70;
            
            // Define warning thresholds for partial reduction (40%, 60%, 75% of max)
            // We use more conservative thresholds for cross-exchange spot-perp
            double warning_threshold_1 = max_allowable_spread * 0.40;  // 40% of max - reduce by 20%
            double warning_threshold_2 = max_allowable_spread * 0.60;  // 60% of max - reduce by 30%
            double warning_threshold_3 = max_allowable_spread * 0.75;  // 75% of max - reduce by 50%
            
            // Skip if spread is within safe range
            if (std::abs(current_spread_pct) < warning_threshold_1) {
                continue;
            }
            
            // Check if funding rate sign flipped (major risk)
            bool is_original_funding_positive = is_spot_long; // If spot is long, original funding was positive
            bool is_current_funding_positive = funding.rate > 0;
            
            if (is_original_funding_positive != is_current_funding_positive) {
                std::cout << "Warning: Funding rate sign has flipped for " << symbol 
                          << ". Planning emergency position reduction." << std::endl;
                
                // Close 75% of the position immediately
                double emergency_reduction = 0.75;
                double perp_reduce_size = std::abs(perp_pos.size) * emergency_reduction;
                double spot_reduce_size = std::abs(spot_size) * emergency_reduction;
                
                OrderSide perp_close_side = perp_pos.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                OrderSide spot_close_side = spot_size > 0 ? OrderSide::SELL : OrderSide::BUY;
                
                // Execute reduction - for emergency, close larger value position first
                if (std::abs(perp_pos.size * perp_price) >= std::abs(spot_size * spot_price)) {
                    perp_exchange_->placeOrder(symbol, perp_close_side, OrderType::MARKET, perp_reduce_size);
                    spot_exchange_->placeOrder(spot_symbol, spot_close_side, OrderType::MARKET, spot_reduce_size);
                } else {
                    spot_exchange_->placeOrder(spot_symbol, spot_close_side, OrderType::MARKET, spot_reduce_size);
                    perp_exchange_->placeOrder(symbol, perp_close_side, OrderType::MARKET, perp_reduce_size);
                }
                
                std::cout << "Emergency position reduction completed for " << symbol << std::endl;
                continue;
            }
            
            // Calculate partial position reduction percentage based on spread
            double reduction_pct = 0.0;
            
            if (std::abs(current_spread_pct) >= warning_threshold_3) {
                reduction_pct = 0.5; // Reduce by 50% at 75% of max spread
                std::cout << "Warning: Spread at 75% of maximum for " << symbol 
                          << ". Reducing position by 50%" << std::endl;
            } 
            else if (std::abs(current_spread_pct) >= warning_threshold_2) {
                reduction_pct = 0.3; // Reduce by 30% at 60% of max spread
                std::cout << "Warning: Spread at 60% of maximum for " << symbol 
                          << ". Reducing position by 30%" << std::endl;
            }
            else if (std::abs(current_spread_pct) >= warning_threshold_1) {
                reduction_pct = 0.2; // Reduce by 20% at 40% of max spread
                std::cout << "Warning: Spread at 40% of maximum for " << symbol 
                          << ". Reducing position by 20%" << std::endl;
            }
            
            // If reduction needed
            if (reduction_pct > 0.0) {
                double perp_reduce_size = std::abs(perp_pos.size) * reduction_pct;
                double spot_reduce_size = std::abs(spot_size) * reduction_pct;
                
                OrderSide perp_close_side = perp_pos.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                OrderSide spot_close_side = spot_size > 0 ? OrderSide::SELL : OrderSide::BUY;
                
                // Determine which position to reduce first based on liquidity
                OrderBook spot_ob = spot_exchange_->getOrderBook(spot_symbol, 3);
                OrderBook perp_ob = perp_exchange_->getOrderBook(symbol, 3);
                
                double spot_liquidity = 0.0;
                double perp_liquidity = 0.0;
                
                // Estimate available liquidity for closing
                if (spot_close_side == OrderSide::SELL) {
                    for (const auto& level : spot_ob.bids) spot_liquidity += level.amount;
                } else {
                    for (const auto& level : spot_ob.asks) spot_liquidity += level.amount;
                }
                
                if (perp_close_side == OrderSide::SELL) {
                    for (const auto& level : perp_ob.bids) perp_liquidity += level.amount;
                } else {
                    for (const auto& level : perp_ob.asks) perp_liquidity += level.amount;
                }
                
                // Close the less liquid position first
                bool close_spot_first = spot_liquidity <= perp_liquidity;
                
                // Execute position reduction
                std::string spot_order_id;
                std::string perp_order_id;
                
                if (close_spot_first) {
                    // Reduce spot first
                    spot_order_id = spot_exchange_->placeOrder(
                        spot_symbol, spot_close_side, OrderType::MARKET, spot_reduce_size);
                    
                    if (spot_order_id.empty()) {
                        std::cerr << "Failed to reduce spot position for " << spot_symbol << std::endl;
                        continue;
                    }
                    
                    // Then reduce perp
                    perp_order_id = perp_exchange_->placeOrder(
                        symbol, perp_close_side, OrderType::MARKET, perp_reduce_size);
                } else {
                    // Reduce perp first
                    perp_order_id = perp_exchange_->placeOrder(
                        symbol, perp_close_side, OrderType::MARKET, perp_reduce_size);
                    
                    if (perp_order_id.empty()) {
                        std::cerr << "Failed to reduce perp position for " << symbol << std::endl;
                        continue;
                    }
                    
                    // Then reduce spot
                    spot_order_id = spot_exchange_->placeOrder(
                        spot_symbol, spot_close_side, OrderType::MARKET, spot_reduce_size);
                }
                
                if (spot_order_id.empty() || perp_order_id.empty()) {
                    std::cerr << "Failed to complete position reduction on both exchanges" << std::endl;
                    continue;
                }
                
                std::cout << "Successfully reduced cross-exchange spot-perp position for " << symbol 
                          << " by " << (reduction_pct * 100.0) << "%. "
                          << "Current spread: " << current_spread_pct << "%, "
                          << "Max allowed: " << max_allowable_spread << "%" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangeSpotPerpStrategy::monitorPositions: " 
                 << e.what() << std::endl;
    }
}

} // namespace funding 