#include <strategy/arbitrage_strategy.h>
#include <exchange/exchange_interface.h>
#include <exchange/types.h>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace funding {

CrossExchangePerpStrategy::CrossExchangePerpStrategy(std::shared_ptr<ExchangeInterface> exchange1,
                                                   std::shared_ptr<ExchangeInterface> exchange2)
    : exchange1_(exchange1), exchange2_(exchange2) {
}

std::vector<ArbitrageOpportunity> CrossExchangePerpStrategy::findOpportunities() {
    std::vector<ArbitrageOpportunity> opportunities;
    
    try {
        // Get all available perpetual futures from both exchanges
        auto perp_instruments1 = exchange1_->getAvailableInstruments(MarketType::PERPETUAL);
        auto perp_instruments2 = exchange2_->getAvailableInstruments(MarketType::PERPETUAL);
        
        // Find matching symbols across exchanges
        for (const auto& perp1 : perp_instruments1) {
            // Find matching instrument on exchange2
            auto perp2_it = std::find_if(perp_instruments2.begin(), perp_instruments2.end(),
                [&perp1](const Instrument& perp2) {
                    return perp2.base_currency == perp1.base_currency && 
                           perp2.quote_currency == perp1.quote_currency;
                });
            
            if (perp2_it == perp_instruments2.end()) {
                continue; // No matching instrument found on exchange2
            }
            
            // Get funding rates for both perpetuals
            FundingRate funding1 = exchange1_->getFundingRate(perp1.symbol);
            FundingRate funding2 = exchange2_->getFundingRate(perp2_it->symbol);
            
            // Calculate funding rate differential
            double funding_diff = funding1.rate - funding2.rate;
            
            // Skip if funding rate differential is too small
            if (std::abs(funding_diff) < 0.0002) { // 0.02% threshold - configurable
                continue;
            }
            
            // Get prices and calculate spread
            double price1 = exchange1_->getPrice(perp1.symbol);
            double price2 = exchange2_->getPrice(perp2_it->symbol);
            double price_spread_pct = (price1 - price2) / price2 * 100.0;
            
            // Create trading pair
            TradingPair pair(exchange1_->getName(), 
                           perp1.symbol, 
                           MarketType::PERPETUAL,
                           exchange2_->getName(),
                           perp2_it->symbol,
                           MarketType::PERPETUAL);
            
            // Calculate annualized funding rates for both exchanges
            double hours_per_year = 24.0 * 365.0;
            double payments_per_year1 = hours_per_year / funding1.payment_interval.count();
            double payments_per_year2 = hours_per_year / funding2.payment_interval.count();
            
            double annualized_rate1 = funding1.rate * payments_per_year1 * 100.0;
            double annualized_rate2 = funding2.rate * payments_per_year2 * 100.0;
            double net_funding_rate = annualized_rate1 - annualized_rate2;
            
            // Get transaction fees for both exchanges
            double taker_fee1 = exchange1_->getTradingFee(perp1.symbol, false);
            double taker_fee2 = exchange2_->getTradingFee(perp2_it->symbol, false);
            
            // Calculate total transaction cost for the complete arbitrage (entry + exit)
            // For each exchange: entry fee + exit fee (as percentage)
            double total_transaction_cost_pct = (taker_fee1 * 2 + taker_fee2 * 2) * 100.0;
            
            // Calculate max allowable spread before it negates funding
            // Use the more conservative payment frequency for calculations
            double min_payments = std::min(payments_per_year1, payments_per_year2);
            double max_spread = std::abs(net_funding_rate) / min_payments * 0.75;
            
            // Build opportunity
            ArbitrageOpportunity opportunity;
            opportunity.pair = pair;
            opportunity.funding_rate1 = funding1.rate;
            opportunity.funding_rate2 = funding2.rate;
            opportunity.net_funding_rate = net_funding_rate;
            opportunity.payment_interval1 = funding1.payment_interval;
            opportunity.payment_interval2 = funding2.payment_interval;
            opportunity.entry_price_spread = price_spread_pct;
            opportunity.max_allowable_spread = max_spread;
            opportunity.transaction_cost_pct = total_transaction_cost_pct;
            
            // Preliminary profit estimate including transaction costs
            // Expected funding profit - price spread cost - transaction costs
            opportunity.estimated_profit = std::abs(net_funding_rate) - std::abs(price_spread_pct) - total_transaction_cost_pct;
            
            // Calculate the number of funding periods needed to break even on transaction costs
            double funding_per_period = std::abs(funding_diff) * 100.0; // As percentage
            opportunity.periods_to_breakeven = total_transaction_cost_pct / funding_per_period;
            
            // Get order books to estimate max position size
            OrderBook ob1 = exchange1_->getOrderBook(perp1.symbol, 10);
            OrderBook ob2 = exchange2_->getOrderBook(perp2_it->symbol, 10);
            
            double liquidity1 = 0.0;
            double liquidity2 = 0.0;
            
            // Calculate available liquidity (simplified)
            // Use bids or asks depending on which side we'd take
            bool long_exchange1 = funding1.rate < funding2.rate;
            
            if (long_exchange1) {
                // Long on exchange 1, short on exchange 2
                // Need ask liquidity on exchange 1, bid liquidity on exchange 2
                for (const auto& level : ob1.asks) {
                    liquidity1 += level.amount * level.price;
                    if (liquidity1 > 50000.0) break; // $50k USD limit
                }
                
                for (const auto& level : ob2.bids) {
                    liquidity2 += level.amount * level.price;
                    if (liquidity2 > 50000.0) break; // $50k USD limit
                }
            } else {
                // Short on exchange 1, long on exchange 2
                // Need bid liquidity on exchange 1, ask liquidity on exchange 2
                for (const auto& level : ob1.bids) {
                    liquidity1 += level.amount * level.price;
                    if (liquidity1 > 50000.0) break; // $50k USD limit
                }
                
                for (const auto& level : ob2.asks) {
                    liquidity2 += level.amount * level.price;
                    if (liquidity2 > 50000.0) break; // $50k USD limit
                }
            }
            
            // Use smaller of the two for max position size
            opportunity.max_position_size = std::min(liquidity1, liquidity2) * 0.25;
            
            // Calculate a basic risk score (0-100), higher is riskier
            double spread_risk = std::abs(price_spread_pct / max_spread) * 40.0; // 40% of score
            double liquidity_risk = (1.0 - std::min(liquidity1, liquidity2) / 50000.0) * 30.0; // 30% of score
            double exchange_risk = 10.0; // Fixed exchange counterparty risk 
            double funding_risk = 20.0; // Higher for cross-exchange due to payment timing mismatches
            
            opportunity.position_risk_score = std::min(100.0, spread_risk + liquidity_risk + exchange_risk + funding_risk);
            
            // Record discovery time
            opportunity.discovery_time = std::chrono::system_clock::now();
            
            // Add to opportunities if estimated profit is positive AFTER transaction costs
            if (opportunity.estimated_profit > 0) {
                opportunities.push_back(opportunity);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangePerpStrategy::findOpportunities: " 
                 << e.what() << std::endl;
    }
    
    // Sort opportunities by estimated profit (highest first)
    std::sort(opportunities.begin(), opportunities.end(),
             [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                 return a.estimated_profit > b.estimated_profit;
             });
    
    return opportunities;
}

bool CrossExchangePerpStrategy::validateOpportunity(const ArbitrageOpportunity& opportunity) {
    try {
        // Get current funding rates
        FundingRate funding1 = exchange1_->getFundingRate(opportunity.pair.symbol1);
        FundingRate funding2 = exchange2_->getFundingRate(opportunity.pair.symbol2);
        
        // Get current prices
        double price1 = exchange1_->getPrice(opportunity.pair.symbol1);
        double price2 = exchange2_->getPrice(opportunity.pair.symbol2);
        
        // Calculate current spread
        double current_spread_pct = (price1 - price2) / price2 * 100.0;
        
        // Check if spread has widened too much
        if (std::abs(current_spread_pct) > opportunity.max_allowable_spread) {
            return false;
        }
        
        // Calculate current annualized funding rates
        double hours_per_year = 24.0 * 365.0;
        double payments_per_year1 = hours_per_year / funding1.payment_interval.count();
        double payments_per_year2 = hours_per_year / funding2.payment_interval.count();
        
        double current_annualized_rate1 = funding1.rate * payments_per_year1 * 100.0;
        double current_annualized_rate2 = funding2.rate * payments_per_year2 * 100.0;
        double current_net_rate = current_annualized_rate1 - current_annualized_rate2;
        
        // If funding rate differential changed by more than 20%, invalidate opportunity
        if (std::abs(current_net_rate - opportunity.net_funding_rate) / 
            std::abs(opportunity.net_funding_rate) > 0.2) {
            return false;
        }
        
        // If funding rates flipped sign, invalidate opportunity
        if ((funding1.rate - funding2.rate) * 
            (opportunity.funding_rate1 - opportunity.funding_rate2) < 0) {
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangePerpStrategy::validateOpportunity: " 
                 << e.what() << std::endl;
        return false;
    }
}

double CrossExchangePerpStrategy::calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) {
    // For cross-exchange, we'll be more conservative with position sizing
    return opportunity.max_position_size * 0.4; // 40% of max
}

bool CrossExchangePerpStrategy::executeTrade(const ArbitrageOpportunity& opportunity, double size) {
    try {
        // Determine direction based on funding rate differential
        // Long the lower funding rate, short the higher funding rate
        bool long_exchange1 = opportunity.funding_rate1 < opportunity.funding_rate2;
        
        // Order sides
        OrderSide side1 = long_exchange1 ? OrderSide::BUY : OrderSide::SELL;
        OrderSide side2 = long_exchange1 ? OrderSide::SELL : OrderSide::BUY;
        
        // Get current prices
        double price1 = exchange1_->getPrice(opportunity.pair.symbol1);
        double price2 = exchange2_->getPrice(opportunity.pair.symbol2);
        
        // Calculate position sizes - we're using USD value, so divide by price
        double size1 = size / price1;
        double size2 = size / price2;
        
        // Execute order on exchange with higher liquidity first (usually safer)
        OrderBook ob1 = exchange1_->getOrderBook(opportunity.pair.symbol1, 3);
        OrderBook ob2 = exchange2_->getOrderBook(opportunity.pair.symbol2, 3);
        
        double liquidity1 = 0.0;
        double liquidity2 = 0.0;
        
        // Simplified liquidity estimation for execution order decision
        if (long_exchange1) {
            for (const auto& level : ob1.asks) liquidity1 += level.amount;
            for (const auto& level : ob2.bids) liquidity2 += level.amount;
        } else {
            for (const auto& level : ob1.bids) liquidity1 += level.amount;
            for (const auto& level : ob2.asks) liquidity2 += level.amount;
        }
        
        // Determine which exchange to execute first based on liquidity
        bool execute_exchange1_first = liquidity1 >= liquidity2;
        
        // Place first order
        std::string order1_id;
        std::string order2_id;
        
        if (execute_exchange1_first) {
            // Execute exchange 1 first
            order1_id = exchange1_->placeOrder(
                opportunity.pair.symbol1,
                side1,
                OrderType::MARKET,
                size1
            );
            
            if (order1_id.empty()) {
                std::cerr << "Failed to place order on " << exchange1_->getName() << std::endl;
                return false;
            }
            
            // Then execute exchange 2
            order2_id = exchange2_->placeOrder(
                opportunity.pair.symbol2,
                side2,
                OrderType::MARKET,
                size2
            );
            
            if (order2_id.empty()) {
                std::cerr << "Failed to place order on " << exchange2_->getName() 
                         << ". Attempting to close position on " << exchange1_->getName() << std::endl;
                
                // Try to close the position on exchange 1
                exchange1_->placeOrder(
                    opportunity.pair.symbol1,
                    side1 == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY,
                    OrderType::MARKET,
                    size1
                );
                
                return false;
            }
        } else {
            // Execute exchange 2 first
            order2_id = exchange2_->placeOrder(
                opportunity.pair.symbol2,
                side2,
                OrderType::MARKET,
                size2
            );
            
            if (order2_id.empty()) {
                std::cerr << "Failed to place order on " << exchange2_->getName() << std::endl;
                return false;
            }
            
            // Then execute exchange 1
            order1_id = exchange1_->placeOrder(
                opportunity.pair.symbol1,
                side1,
                OrderType::MARKET,
                size1
            );
            
            if (order1_id.empty()) {
                std::cerr << "Failed to place order on " << exchange1_->getName() 
                         << ". Attempting to close position on " << exchange2_->getName() << std::endl;
                
                // Try to close the position on exchange 2
                exchange2_->placeOrder(
                    opportunity.pair.symbol2,
                    side2 == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY,
                    OrderType::MARKET,
                    size2
                );
                
                return false;
            }
        }
        
        std::cout << "Successfully executed cross-exchange arbitrage trade for " 
                 << opportunity.pair.symbol1 << " on " << exchange1_->getName()
                 << " and " << opportunity.pair.symbol2 << " on " << exchange2_->getName()
                 << " with size " << size << " USD" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangePerpStrategy::executeTrade: " 
                 << e.what() << std::endl;
        return false;
    }
}

bool CrossExchangePerpStrategy::closePosition(const ArbitrageOpportunity& opportunity) {
    try {
        // Get current positions
        auto positions1 = exchange1_->getPositions();
        auto positions2 = exchange2_->getPositions();
        
        // Find matching positions
        auto pos1 = std::find_if(positions1.begin(), positions1.end(),
                               [&opportunity](const Position& pos) {
                                   return pos.symbol == opportunity.pair.symbol1;
                               });
        
        auto pos2 = std::find_if(positions2.begin(), positions2.end(),
                               [&opportunity](const Position& pos) {
                                   return pos.symbol == opportunity.pair.symbol2;
                               });
        
        if (pos1 == positions1.end() || pos2 == positions2.end()) {
            std::cerr << "Could not find matching positions to close" << std::endl;
            return false;
        }
        
        // Determine closing sides
        OrderSide close_side1 = pos1->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        OrderSide close_side2 = pos2->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        
        double close_size1 = std::abs(pos1->size);
        double close_size2 = std::abs(pos2->size);
        
        // Close the less liquid exchange first (to reduce slippage risk on the more liquid one)
        OrderBook ob1 = exchange1_->getOrderBook(opportunity.pair.symbol1, 3);
        OrderBook ob2 = exchange2_->getOrderBook(opportunity.pair.symbol2, 3);
        
        double liquidity1 = 0.0;
        double liquidity2 = 0.0;
        
        // Simplified liquidity estimation
        if (close_side1 == OrderSide::SELL) {
            for (const auto& level : ob1.bids) liquidity1 += level.amount;
        } else {
            for (const auto& level : ob1.asks) liquidity1 += level.amount;
        }
        
        if (close_side2 == OrderSide::SELL) {
            for (const auto& level : ob2.bids) liquidity2 += level.amount;
        } else {
            for (const auto& level : ob2.asks) liquidity2 += level.amount;
        }
        
        // Close the less liquid exchange first
        bool close_exchange1_first = liquidity1 <= liquidity2;
        
        if (close_exchange1_first) {
            // Close exchange 1 first
            std::string close1_id = exchange1_->placeOrder(
                opportunity.pair.symbol1,
                close_side1,
                OrderType::MARKET,
                close_size1
            );
            
            if (close1_id.empty()) {
                std::cerr << "Failed to close position on " << exchange1_->getName() << std::endl;
                return false;
            }
            
            // Then close exchange 2
            std::string close2_id = exchange2_->placeOrder(
                opportunity.pair.symbol2,
                close_side2,
                OrderType::MARKET,
                close_size2
            );
            
            if (close2_id.empty()) {
                std::cerr << "Failed to close position on " << exchange2_->getName() << std::endl;
                return false;
            }
        } else {
            // Close exchange 2 first
            std::string close2_id = exchange2_->placeOrder(
                opportunity.pair.symbol2,
                close_side2,
                OrderType::MARKET,
                close_size2
            );
            
            if (close2_id.empty()) {
                std::cerr << "Failed to close position on " << exchange2_->getName() << std::endl;
                return false;
            }
            
            // Then close exchange 1
            std::string close1_id = exchange1_->placeOrder(
                opportunity.pair.symbol1,
                close_side1,
                OrderType::MARKET,
                close_size1
            );
            
            if (close1_id.empty()) {
                std::cerr << "Failed to close position on " << exchange1_->getName() << std::endl;
                return false;
            }
        }
        
        std::cout << "Successfully closed cross-exchange arbitrage positions for " 
                 << opportunity.pair.symbol1 << " on " << exchange1_->getName()
                 << " and " << opportunity.pair.symbol2 << " on " << exchange2_->getName() << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangePerpStrategy::closePosition: " 
                 << e.what() << std::endl;
        return false;
    }
}

void CrossExchangePerpStrategy::monitorPositions() {
    try {
        // Get positions from both exchanges
        auto positions1 = exchange1_->getPositions();
        auto positions2 = exchange2_->getPositions();
        
        // Create a map of symbol to position for exchange 2
        std::map<std::string, Position> positions2_map;
        for (const auto& pos : positions2) {
            positions2_map[pos.symbol] = pos;
        }
        
        // For each position on exchange 1, find the matching hedge on exchange 2
        for (const auto& pos1 : positions1) {
            // Skip small/dust positions
            if (std::abs(pos1.size) * pos1.entry_price < 10.0) { // $10 minimum
                continue;
            }
            
            std::string symbol1 = pos1.symbol;
            
            // Find corresponding position on exchange 2
            // This is a simplified approach - in real implementation we would have a tracking system
            auto pos2_it = positions2_map.find(symbol1);
            if (pos2_it == positions2_map.end() || std::abs(pos2_it->second.size) * pos2_it->second.entry_price < 10.0) {
                continue; // No matching position
            }
            
            Position pos2 = pos2_it->second;
            
            // Verify positions are in opposite directions (hedged)
            if ((pos1.size > 0 && pos2.size > 0) || (pos1.size < 0 && pos2.size < 0)) {
                std::cerr << "Warning: Positions on " << symbol1 << " are not properly hedged" << std::endl;
                continue;
            }
            
            // Get current prices
            double price1 = exchange1_->getPrice(symbol1);
            double price2 = exchange2_->getPrice(symbol1);
            
            // Calculate current spread
            double current_spread_pct = (price1 - price2) / price2 * 100.0;
            
            // Get funding rates for both perpetuals
            FundingRate funding1 = exchange1_->getFundingRate(symbol1);
            FundingRate funding2 = exchange2_->getFundingRate(symbol1);
            
            // Calculate annualized rates
            double hours_per_year = 24.0 * 365.0;
            double payments_per_year1 = hours_per_year / funding1.payment_interval.count();
            double payments_per_year2 = hours_per_year / funding2.payment_interval.count();
            
            double annualized_rate1 = funding1.rate * payments_per_year1 * 100.0;
            double annualized_rate2 = funding2.rate * payments_per_year2 * 100.0;
            double net_funding_rate = annualized_rate1 - annualized_rate2;
            
            // Calculate max allowable spread
            double min_payments = std::min(payments_per_year1, payments_per_year2);
            double max_allowable_spread = std::abs(net_funding_rate) / min_payments * 0.75;
            
            // Define spread warning thresholds (45%, 65%, 80% of max)
            double warning_threshold_1 = max_allowable_spread * 0.45;  // 45% of max - reduce by 20%
            double warning_threshold_2 = max_allowable_spread * 0.65;  // 65% of max - reduce by 30%
            double warning_threshold_3 = max_allowable_spread * 0.80;  // 80% of max - reduce by 50%
            
            // For cross-exchange, we're more conservative with thresholds
            
            // Skip if the current spread is small (in safe zone)
            if (std::abs(current_spread_pct) < warning_threshold_1) {
                continue;
            }
            
            // Check for funding rate sign change (major risk)
            bool funding_sign_flipped = (funding1.rate - funding2.rate) * (pos1.size) > 0;
            if (funding_sign_flipped) {
                std::cout << "Warning: Funding rate sign has flipped for " << symbol1 
                          << ". Planning emergency position reduction." << std::endl;
                
                // Close 70% of the position immediately as funding rate direction change is dangerous
                double emergency_reduction = 0.7;
                double pos1_reduce_size = std::abs(pos1.size) * emergency_reduction;
                double pos2_reduce_size = std::abs(pos2.size) * emergency_reduction;
                
                OrderSide close_side1 = pos1.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                OrderSide close_side2 = pos2.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                
                // Close larger position first to reduce risk exposure
                if (std::abs(pos1.size * price1) >= std::abs(pos2.size * price2)) {
                    exchange1_->placeOrder(symbol1, close_side1, OrderType::MARKET, pos1_reduce_size);
                    exchange2_->placeOrder(symbol1, close_side2, OrderType::MARKET, pos2_reduce_size);
                } else {
                    exchange2_->placeOrder(symbol1, close_side2, OrderType::MARKET, pos2_reduce_size);
                    exchange1_->placeOrder(symbol1, close_side1, OrderType::MARKET, pos1_reduce_size);
                }
                
                std::cout << "Emergency position reduction completed for " << symbol1 << std::endl;
                continue;
            }
            
            // Partial position reduction logic based on spread thresholds
            double reduction_pct = 0.0;
            
            if (std::abs(current_spread_pct) >= warning_threshold_3) {
                reduction_pct = 0.5; // Reduce by 50% at 80% of max spread
                std::cout << "Warning: Spread at 80% of maximum for " << symbol1 
                          << ". Reducing position by 50%" << std::endl;
            } 
            else if (std::abs(current_spread_pct) >= warning_threshold_2) {
                reduction_pct = 0.3; // Reduce by 30% at 65% of max spread
                std::cout << "Warning: Spread at 65% of maximum for " << symbol1 
                          << ". Reducing position by 30%" << std::endl;
            }
            else if (std::abs(current_spread_pct) >= warning_threshold_1) {
                reduction_pct = 0.2; // Reduce by 20% at 45% of max spread
                std::cout << "Warning: Spread at 45% of maximum for " << symbol1 
                          << ". Reducing position by 20%" << std::endl;
            }
            
            // If we need to reduce position
            if (reduction_pct > 0.0) {
                double pos1_reduce_size = std::abs(pos1.size) * reduction_pct;
                double pos2_reduce_size = std::abs(pos2.size) * reduction_pct;
                
                OrderSide close_side1 = pos1.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                OrderSide close_side2 = pos2.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                
                // Check liquidity on both sides to determine which to close first
                OrderBook ob1 = exchange1_->getOrderBook(symbol1, 3);
                OrderBook ob2 = exchange2_->getOrderBook(symbol1, 3);
                
                double liquidity1 = 0.0;
                double liquidity2 = 0.0;
                
                // Estimate liquidity for close direction
                if (close_side1 == OrderSide::SELL) {
                    for (const auto& level : ob1.bids) liquidity1 += level.amount;
                } else {
                    for (const auto& level : ob1.asks) liquidity1 += level.amount;
                }
                
                if (close_side2 == OrderSide::SELL) {
                    for (const auto& level : ob2.bids) liquidity2 += level.amount;
                } else {
                    for (const auto& level : ob2.asks) liquidity2 += level.amount;
                }
                
                // Close less liquid exchange first for gradual reduction
                bool close_exchange1_first = liquidity1 <= liquidity2;
                
                // Execute reductions
                std::string order1_id;
                std::string order2_id;
                
                if (close_exchange1_first) {
                    order1_id = exchange1_->placeOrder(
                        symbol1, close_side1, OrderType::MARKET, pos1_reduce_size);
                    
                    if (order1_id.empty()) {
                        std::cerr << "Failed to reduce position on " << exchange1_->getName() << std::endl;
                        continue;
                    }
                    
                    order2_id = exchange2_->placeOrder(
                        symbol1, close_side2, OrderType::MARKET, pos2_reduce_size);
                } else {
                    order2_id = exchange2_->placeOrder(
                        symbol1, close_side2, OrderType::MARKET, pos2_reduce_size);
                    
                    if (order2_id.empty()) {
                        std::cerr << "Failed to reduce position on " << exchange2_->getName() << std::endl;
                        continue;
                    }
                    
                    order1_id = exchange1_->placeOrder(
                        symbol1, close_side1, OrderType::MARKET, pos1_reduce_size);
                }
                
                if (order1_id.empty() || order2_id.empty()) {
                    std::cerr << "Failed to complete position reduction on both exchanges" << std::endl;
                    continue;
                }
                
                std::cout << "Successfully reduced cross-exchange position for " << symbol1 
                          << " by " << (reduction_pct * 100.0) << "%. "
                          << "Current spread: " << current_spread_pct << "%, "
                          << "Max allowed: " << max_allowable_spread << "%" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in CrossExchangePerpStrategy::monitorPositions: " 
                 << e.what() << std::endl;
    }
}

} // namespace funding 