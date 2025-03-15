#include <strategy/arbitrage_strategy.h>
#include <exchange/exchange_interface.h>
#include <exchange/types.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <set>

namespace funding {

CrossExchangeSpotPerpStrategy::CrossExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> spot_exchange,
                                                           std::shared_ptr<ExchangeInterface> perp_exchange)
    : spot_exchange_(spot_exchange), perp_exchange_(perp_exchange) {
    std::stringstream ss;
    ss << "Initialized with spot exchange: " << spot_exchange_->getName() 
       << " and perp exchange: " << perp_exchange_->getName();
    std::cout << ss.str() << std::endl;
}

std::vector<ArbitrageOpportunity> CrossExchangeSpotPerpStrategy::findOpportunities() {
    std::vector<ArbitrageOpportunity> opportunities;
    
    try {
        std::cout << "CrossExchangeSpotPerpStrategy::findOpportunities - Starting for exchanges: " 
                  << spot_exchange_->getName() << " (spot) and " 
                  << perp_exchange_->getName() << " (perp)" << std::endl;
        
        // Get all available instruments from both exchanges with error handling
        std::vector<Instrument> perp_instruments;
        std::vector<Instrument> spot_instruments;
        
        try {
            std::cout << "Getting perpetual instruments from " << perp_exchange_->getName() << std::endl;
            perp_instruments = perp_exchange_->getAvailableInstruments(MarketType::PERPETUAL);
            
            if (perp_instruments.empty()) {
                std::cout << "No perpetual instruments found on " << perp_exchange_->getName() << std::endl;
                return opportunities;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching perpetual instruments from " << perp_exchange_->getName() 
                      << ": " << e.what() << std::endl;
            return opportunities;
        }
        
        try {
            std::cout << "Getting spot instruments from " << spot_exchange_->getName() << std::endl;
            spot_instruments = spot_exchange_->getAvailableInstruments(MarketType::SPOT);
            
            if (spot_instruments.empty()) {
                std::cout << "No spot instruments found on " << spot_exchange_->getName() << std::endl;
                return opportunities;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching spot instruments from " << spot_exchange_->getName() 
                      << ": " << e.what() << std::endl;
            return opportunities;
        }
        
        std::stringstream ss;
        ss << "Found " << perp_instruments.size() << " perpetual instruments on " << perp_exchange_->getName()
           << " and " << spot_instruments.size() << " spot instruments on " << spot_exchange_->getName();
        std::cout << ss.str() << std::endl;
        
        // Process each perpetual instrument
        for (const auto& perp : perp_instruments) {
            try {
                // Skip instruments with empty base or quote currency
                if (perp.base_currency.empty() || perp.quote_currency.empty()) {
                    std::cout << "Skipping " << perp.symbol << " - empty base or quote currency" << std::endl;
                    continue;
                }
                
                // Find matching spot instrument
                auto spot_it = std::find_if(spot_instruments.begin(), spot_instruments.end(),
                    [&perp](const Instrument& spot) {
                        return !spot.base_currency.empty() && !spot.quote_currency.empty() &&
                               spot.base_currency == perp.base_currency && 
                               spot.quote_currency == perp.quote_currency;
                    });
                
                if (spot_it == spot_instruments.end()) {
                    // No matching spot instrument found
                    continue;
                }
                
                // Get funding rate for perpetual instrument
                FundingRate funding;
                try {
                    funding = perp_exchange_->getFundingRate(perp.symbol);
                    
                    // Skip if funding rate is too small
                    if (std::abs(funding.rate) < min_funding_rate_) {
                        continue;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error getting funding rate for " << perp.symbol << " on " 
                              << perp_exchange_->getName() << ": " << e.what() << std::endl;
                    continue;
                }
                
                // Calculate annualized funding rate
                double hours_per_year = 24.0 * 365.0;
                double payments_per_year = hours_per_year / funding.payment_interval.count();
                double annual_rate = funding.rate * payments_per_year * 100.0; // Convert to percentage
                
                // Get current prices
                double spot_price = 0.0;
                double perp_price = 0.0;
                
                try {
                    spot_price = spot_exchange_->getPrice(spot_it->symbol);
                    if (spot_price <= 0.0) {
                        std::cerr << "Invalid spot price for " << spot_it->symbol << ": " << spot_price << std::endl;
                        continue;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error getting spot price for " << spot_it->symbol << ": " << e.what() << std::endl;
                    continue;
                }
                
                try {
                    perp_price = perp_exchange_->getPrice(perp.symbol);
                    if (perp_price <= 0.0) {
                        std::cerr << "Invalid perp price for " << perp.symbol << ": " << perp_price << std::endl;
                        continue;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error getting perp price for " << perp.symbol << ": " << e.what() << std::endl;
                    continue;
                }
                
                // Calculate price spread as percentage
                double price_spread_pct = (perp_price - spot_price) / spot_price * 100.0;
                
                // Get trading fees
                double spot_maker_fee = 0.001; // Default 0.1%
                double spot_taker_fee = 0.001; // Default 0.1%
                double perp_maker_fee = 0.0002; // Default 0.02%
                double perp_taker_fee = 0.0005; // Default 0.05%
                
                try {
                    spot_maker_fee = spot_exchange_->getTradingFee(spot_it->symbol, true);
                    spot_taker_fee = spot_exchange_->getTradingFee(spot_it->symbol, false);
                } catch (const std::exception& e) {
                    std::cerr << "Error getting spot trading fees for " << spot_it->symbol 
                              << ", using defaults: " << e.what() << std::endl;
                }
                
                try {
                    perp_maker_fee = perp_exchange_->getTradingFee(perp.symbol, true);
                    perp_taker_fee = perp_exchange_->getTradingFee(perp.symbol, false);
                } catch (const std::exception& e) {
                    std::cerr << "Error getting perp trading fees for " << perp.symbol 
                              << ", using defaults: " << e.what() << std::endl;
                }
                
                // Calculate total transaction cost (entry + exit)
                double transaction_cost_pct = (spot_taker_fee + perp_taker_fee) * 2 * 100.0; // Convert to percentage
                
                // Calculate estimated profit
                double estimated_profit = std::abs(annual_rate) - transaction_cost_pct;
                
                // Skip if estimated profit is too low
                if (estimated_profit < min_expected_profit_) {
                    continue;
                }
                
                // Calculate periods to breakeven
                double periods_to_breakeven = transaction_cost_pct / std::abs(funding.rate * 100.0);
                
                // Calculate maximum allowable spread based on funding rate
                double max_spread = std::abs(annual_rate) * 0.1; // 10% of annualized funding rate
                
                // Skip if current spread is too wide
                if (std::abs(price_spread_pct) > max_spread) {
                    continue;
                }
                
                // Get order books to estimate max position size
                double spot_liquidity = 50000.0; // Default $50k
                double perp_liquidity = 50000.0; // Default $50k
                
                try {
                    OrderBook spot_ob = spot_exchange_->getOrderBook(spot_it->symbol, 5);
                    spot_liquidity = 0.0;
                    
                    // Determine which side of the order book to use based on funding rate
                    if (funding.rate > 0) {
                        // Long spot, short perp
                        for (const auto& level : spot_ob.asks) {
                            spot_liquidity += level.amount * level.price;
                        }
                    } else {
                        // Short spot, long perp
                        for (const auto& level : spot_ob.bids) {
                            spot_liquidity += level.amount * level.price;
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error getting spot order book for " << spot_it->symbol 
                              << ", using default liquidity: " << e.what() << std::endl;
                }
                
                try {
                    OrderBook perp_ob = perp_exchange_->getOrderBook(perp.symbol, 5);
                    perp_liquidity = 0.0;
                    
                    // Determine which side of the order book to use based on funding rate
                    if (funding.rate > 0) {
                        // Long spot, short perp
                        for (const auto& level : perp_ob.bids) {
                            perp_liquidity += level.amount * level.price;
                        }
                    } else {
                        // Short spot, long perp
                        for (const auto& level : perp_ob.asks) {
                            perp_liquidity += level.amount * level.price;
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error getting perp order book for " << perp.symbol 
                              << ", using default liquidity: " << e.what() << std::endl;
                }
                
                // Use the minimum liquidity as the constraint
                double max_position_size = std::min(spot_liquidity, perp_liquidity) * 0.2; // Use 20% of available liquidity
                max_position_size = std::min(max_position_size, 10000.0); // Cap at $10k
                
                // Set a minimum position size to avoid dust trades
                if (max_position_size < 10.0) {
                    std::cout << "Skipping " << perp.symbol << " - insufficient liquidity for meaningful position" << std::endl;
                    continue;
                }
                
                // Create opportunity
                ArbitrageOpportunity opportunity;
                opportunity.pair.exchange1 = spot_exchange_->getName();
                opportunity.pair.symbol1 = spot_it->symbol;
                opportunity.pair.market_type1 = MarketType::SPOT;
                opportunity.pair.exchange2 = perp_exchange_->getName();
                opportunity.pair.symbol2 = perp.symbol;
                opportunity.pair.market_type2 = MarketType::PERPETUAL;
                opportunity.funding_rate1 = 0.0; // Spot doesn't pay funding
                opportunity.funding_rate2 = funding.rate;
                opportunity.net_funding_rate = annual_rate;
                opportunity.payment_interval1 = std::chrono::hours(0);
                opportunity.payment_interval2 = funding.payment_interval;
                opportunity.entry_price_spread = price_spread_pct;
                opportunity.max_allowable_spread = max_spread;
                opportunity.transaction_cost_pct = transaction_cost_pct;
                opportunity.estimated_profit = estimated_profit;
                opportunity.periods_to_breakeven = periods_to_breakeven;
                opportunity.max_position_size = max_position_size;
                
                // Calculate risk score
                double spread_risk = std::abs(price_spread_pct / max_spread) * 40.0; // 40% of score
                double liquidity_risk = (1.0 - std::min(spot_liquidity, perp_liquidity) / 50000.0) * 25.0; // 25% of score
                double exchange_risk = 15.0; // Higher exchange counterparty risk for cross-exchange
                double funding_risk = 20.0; // Base funding risk (would be better with historical data)
                
                opportunity.position_risk_score = std::min(100.0, spread_risk + liquidity_risk + exchange_risk + funding_risk);
                
                // Record discovery time
                opportunity.discovery_time = std::chrono::system_clock::now();
                
                // Set the strategy type
                opportunity.strategy_type = "CrossExchangeSpotPerpStrategy";
                opportunity.strategy_index = -1; // Will be set by CompositeStrategy if used
                
                // Add to opportunities if estimated profit is positive AFTER transaction costs
                if (opportunity.estimated_profit > 0) {
                    opportunities.push_back(opportunity);
                    
                    // Log opportunity details
                    std::stringstream opportunity_ss;
                    opportunity_ss << "Found opportunity: " << spot_it->symbol << " on " << spot_exchange_->getName()
                       << " vs " << perp.symbol << " on " << perp_exchange_->getName()
                       << " | Funding rate: " << (funding.rate * 100.0) << "%"
                       << " | Spread: " << price_spread_pct << "%"
                       << " | Est. profit: " << opportunity.estimated_profit << "%"
                       << " | Risk score: " << opportunity.position_risk_score;
                    std::cout << opportunity_ss.str() << std::endl;
                }
            } catch (const std::exception& e) {
                std::stringstream error_ss;
                error_ss << "Error processing perpetual instrument from " << perp_exchange_->getName() 
                         << ": " << perp.symbol << " - " << e.what();
                std::cerr << error_ss.str() << std::endl;
            }
        }
        
        // Sort opportunities by estimated profit
        std::sort(opportunities.begin(), opportunities.end(),
                 [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                     return a.estimated_profit > b.estimated_profit;
                 });
        
        std::cout << "Found " << opportunities.size() << " opportunities for " 
                  << spot_exchange_->getName() << " (spot) and " 
                  << perp_exchange_->getName() << " (perp)" << std::endl;
        
    } catch (const std::exception& e) {
        std::stringstream error_ss;
        error_ss << "Error in CrossExchangeSpotPerpStrategy::findOpportunities: " << e.what();
        std::cerr << error_ss.str() << std::endl;
    }
    
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
            std::stringstream ss;
            ss << "Spread too large: current " << current_spread_pct 
               << "%, max allowed " << opportunity.max_allowable_spread << "%";
            std::cout << ss.str() << std::endl;
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
            std::stringstream ss;
            ss << "Funding rate changed significantly: was " << opportunity.net_funding_rate 
               << "%, now " << current_annualized_rate << "%";
            std::cout << ss.str() << std::endl;
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error in CrossExchangeSpotPerpStrategy::validateOpportunity: " 
           << e.what();
        std::cerr << ss.str() << std::endl;
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
        
        // Create order objects
        Order spot_order;
        spot_order.symbol = opportunity.pair.symbol1;
        spot_order.side = spot_side;
        spot_order.type = OrderType::MARKET;
        spot_order.amount = spot_size;
        spot_order.price = 0; // Market order
        
        Order perp_order;
        perp_order.symbol = opportunity.pair.symbol2;
        perp_order.side = perp_side;
        perp_order.type = OrderType::MARKET;
        perp_order.amount = perp_size;
        perp_order.price = 0; // Market order
        
        if (execute_spot_first) {
            // Execute spot order first
            spot_order_id = spot_exchange_->placeOrder(spot_order);
            
            if (spot_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to place spot order on " 
                   << spot_exchange_->getName();
                std::cerr << ss.str() << std::endl;
                return false;
            }
            
            // Execute perp order
            perp_order_id = perp_exchange_->placeOrder(perp_order);
            
            if (perp_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to place perp order on " << perp_exchange_->getName() 
                   << ". Attempting to close spot position.";
                std::cerr << ss.str() << std::endl;
                
                // Try to close the spot position
                Order spot_close_order;
                spot_close_order.symbol = opportunity.pair.symbol1;
                spot_close_order.side = spot_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
                spot_close_order.type = OrderType::MARKET;
                spot_close_order.amount = spot_size;
                
                spot_exchange_->placeOrder(spot_close_order);
                
                return false;
            }
        } else {
            // Execute perp order first
            perp_order_id = perp_exchange_->placeOrder(perp_order);
            
            if (perp_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to place perp order on " 
                   << perp_exchange_->getName();
                std::cerr << ss.str() << std::endl;
                return false;
            }
            
            // Execute spot order
            spot_order_id = spot_exchange_->placeOrder(spot_order);
            
            if (spot_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to place spot order on " << spot_exchange_->getName() 
                   << ". Attempting to close perp position.";
                std::cerr << ss.str() << std::endl;
                
                // Try to close the perp position
                Order perp_close_order;
                perp_close_order.symbol = opportunity.pair.symbol2;
                perp_close_order.side = perp_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
                perp_close_order.type = OrderType::MARKET;
                perp_close_order.amount = perp_size;
                
                perp_exchange_->placeOrder(perp_close_order);
                
                return false;
            }
        }
        
        std::stringstream ss;
        ss << "Successfully executed cross-exchange spot-perp arbitrage trade for " 
           << opportunity.pair.symbol1 << " on " << spot_exchange_->getName()
           << " and " << opportunity.pair.symbol2 << " on " << perp_exchange_->getName()
           << " with size " << size << " USD";
        std::cout << ss.str() << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error in CrossExchangeSpotPerpStrategy::executeTrade: " 
           << e.what();
        std::cerr << ss.str() << std::endl;
        return false;
    }
}

bool CrossExchangeSpotPerpStrategy::closePosition(const ArbitrageOpportunity& opportunity) {
    try {
        // Get perp positions
        auto perp_positions = perp_exchange_->getOpenPositions();
        
        // Find matching perpetual position
        auto perp_pos = std::find_if(perp_positions.begin(), perp_positions.end(),
                                    [&opportunity](const Position& pos) {
                                        return pos.symbol == opportunity.pair.symbol2;
                                    });
        
        if (perp_pos == perp_positions.end()) {
            std::stringstream ss;
            ss << "No open perpetual position found for " 
               << opportunity.pair.symbol2;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Determine closing orders
        OrderSide perp_close_side = perp_pos->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        double perp_size = std::abs(perp_pos->size);
        
        // Get spot balances to find spot position
        auto account_balance = spot_exchange_->getAccountBalance();
        std::string base_currency = opportunity.pair.symbol1.substr(0, opportunity.pair.symbol1.find('/'));
        
        // Check if we have the currency in our balance
        double spot_size = 0.0;
        if (account_balance.total.find(base_currency) != account_balance.total.end()) {
            spot_size = account_balance.total[base_currency];
        }
        
        if (spot_size < 0.00001) {
            std::stringstream ss;
            ss << "No " << base_currency << " balance found on spot exchange";
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
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
        
        // Create order objects
        Order spot_close_order;
        spot_close_order.symbol = opportunity.pair.symbol1;
        spot_close_order.side = spot_close_side;
        spot_close_order.type = OrderType::MARKET;
        spot_close_order.amount = spot_size;
        
        Order perp_close_order;
        perp_close_order.symbol = opportunity.pair.symbol2;
        perp_close_order.side = perp_close_side;
        perp_close_order.type = OrderType::MARKET;
        perp_close_order.amount = perp_size;
        
        // Close less liquid position first
        bool close_spot_first = spot_liquidity <= perp_liquidity;
        
        if (close_spot_first) {
            // Close spot position first
            std::string spot_order_id = spot_exchange_->placeOrder(spot_close_order);
            
            if (spot_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to close spot position on " << spot_exchange_->getName();
                std::cerr << ss.str() << std::endl;
                return false;
            }
            
            // Then close perp position
            std::string perp_order_id = perp_exchange_->placeOrder(perp_close_order);
            
            if (perp_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to close perpetual position on " << perp_exchange_->getName();
                std::cerr << ss.str() << std::endl;
                return false;
            }
        } else {
            // Close perp position first
            std::string perp_order_id = perp_exchange_->placeOrder(perp_close_order);
            
            if (perp_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to close perpetual position on " << perp_exchange_->getName();
                std::cerr << ss.str() << std::endl;
                return false;
            }
            
            // Then close spot position
            std::string spot_order_id = spot_exchange_->placeOrder(spot_close_order);
            
            if (spot_order_id.empty()) {
                std::stringstream ss;
                ss << "Failed to close spot position on " << spot_exchange_->getName();
                std::cerr << ss.str() << std::endl;
                return false;
            }
        }
        
        std::stringstream ss;
        ss << "Successfully closed cross-exchange spot-perp arbitrage position for " 
           << opportunity.pair.symbol1 << " on " << spot_exchange_->getName()
           << " and " << opportunity.pair.symbol2 << " on " << perp_exchange_->getName();
        std::cout << ss.str() << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error in CrossExchangeSpotPerpStrategy::closePosition: " 
           << e.what();
        std::cerr << ss.str() << std::endl;
        return false;
    }
}

void CrossExchangeSpotPerpStrategy::monitorPositions() {
    try {
        // Get perp positions
        auto perp_positions = perp_exchange_->getOpenPositions();
        
        // Get spot balances
        auto account_balance = spot_exchange_->getAccountBalance();
        
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
            double spot_size = 0.0;
            if (account_balance.total.find(base_currency) != account_balance.total.end()) {
                spot_size = account_balance.total[base_currency];
            }
            
            if (std::abs(spot_size) < 0.00001) {
                continue; // No matching spot position
            }
            
            // Verify spot and perp positions have opposite signs (proper hedge)
            bool is_perp_long = perp_pos.size > 0;
            bool is_spot_long = spot_size > 0;
            
            if ((is_perp_long && is_spot_long) || (!is_perp_long && !is_spot_long)) {
                std::stringstream ss;
                ss << "Warning: Positions on " << symbol << " are not properly hedged";
                std::cerr << ss.str() << std::endl;
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
                std::stringstream ss;
                ss << "Warning: Funding rate sign has flipped for " << symbol
                   << ". Planning emergency position reduction.";
                std::cout << ss.str() << std::endl;
                
                // Close 75% of the position immediately
                double emergency_reduction = 0.75;
                double perp_reduce_size = std::abs(perp_pos.size) * emergency_reduction;
                double spot_reduce_size = std::abs(spot_size) * emergency_reduction;
                
                OrderSide perp_close_side = perp_pos.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                OrderSide spot_close_side = spot_size > 0 ? OrderSide::SELL : OrderSide::BUY;
                
                // Create order objects
                Order perp_order;
                perp_order.symbol = symbol;
                perp_order.side = perp_close_side;
                perp_order.type = OrderType::MARKET;
                perp_order.amount = perp_reduce_size;
                
                Order spot_order;
                spot_order.symbol = spot_symbol;
                spot_order.side = spot_close_side;
                spot_order.type = OrderType::MARKET;
                spot_order.amount = spot_reduce_size;
                
                // Execute reduction - for emergency, close larger value position first
                if (std::abs(perp_pos.size * perp_price) >= std::abs(spot_size * spot_price)) {
                    perp_exchange_->placeOrder(perp_order);
                    spot_exchange_->placeOrder(spot_order);
                } else {
                    spot_exchange_->placeOrder(spot_order);
                    perp_exchange_->placeOrder(perp_order);
                }
                
                std::stringstream completion_ss;
                completion_ss << "Emergency position reduction completed for " << symbol;
                std::cout << completion_ss.str() << std::endl;
                continue;
            }
            
            // Calculate partial position reduction percentage based on spread
            double reduction_pct = 0.0;
            
            if (std::abs(current_spread_pct) >= warning_threshold_3) {
                reduction_pct = 0.5; // Reduce by 50% at 75% of max spread
                std::stringstream ss;
                ss << "Warning: Spread at 75% of maximum for " << symbol
                   << ". Reducing position by 50%";
                std::cout << ss.str() << std::endl;
            } 
            else if (std::abs(current_spread_pct) >= warning_threshold_2) {
                reduction_pct = 0.3; // Reduce by 30% at 60% of max spread
                std::stringstream ss;
                ss << "Warning: Spread at 60% of maximum for " << symbol
                   << ". Reducing position by 30%";
                std::cout << ss.str() << std::endl;
            }
            else if (std::abs(current_spread_pct) >= warning_threshold_1) {
                reduction_pct = 0.2; // Reduce by 20% at 40% of max spread
                std::stringstream ss;
                ss << "Warning: Spread at 40% of maximum for " << symbol
                   << ". Reducing position by 20%";
                std::cout << ss.str() << std::endl;
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
                
                // Create order objects
                Order spot_order;
                spot_order.symbol = spot_symbol;
                spot_order.side = spot_close_side;
                spot_order.type = OrderType::MARKET;
                spot_order.amount = spot_reduce_size;
                
                Order perp_order;
                perp_order.symbol = symbol;
                perp_order.side = perp_close_side;
                perp_order.type = OrderType::MARKET;
                perp_order.amount = perp_reduce_size;
                
                // Close the less liquid position first
                bool close_spot_first = spot_liquidity <= perp_liquidity;
                
                // Execute position reduction
                std::string spot_order_id;
                std::string perp_order_id;
                
                if (close_spot_first) {
                    // Reduce spot first
                    spot_order_id = spot_exchange_->placeOrder(spot_order);
                    
                    if (spot_order_id.empty()) {
                        std::stringstream error_ss;
                        error_ss << "Failed to reduce spot position for " << spot_symbol;
                        std::cerr << error_ss.str() << std::endl;
                        continue;
                    }
                    
                    // Then reduce perp
                    perp_order_id = perp_exchange_->placeOrder(perp_order);
                } else {
                    // Reduce perp first
                    perp_order_id = perp_exchange_->placeOrder(perp_order);
                    
                    if (perp_order_id.empty()) {
                        std::stringstream error_ss;
                        error_ss << "Failed to reduce perp position for " << symbol;
                        std::cerr << error_ss.str() << std::endl;
                        continue;
                    }
                    
                    // Then reduce spot
                    spot_order_id = spot_exchange_->placeOrder(spot_order);
                }
                
                if (spot_order_id.empty() || perp_order_id.empty()) {
                    std::stringstream error_ss;
                    error_ss << "Failed to complete position reduction on both exchanges";
                    std::cerr << error_ss.str() << std::endl;
                    continue;
                }
                
                std::stringstream ss;
                ss << "Successfully reduced cross-exchange spot-perp position for " << symbol 
                   << " by " << (reduction_pct * 100.0) << "%. "
                   << "Current spread: " << current_spread_pct << "%, "
                   << "Max allowed: " << max_allowable_spread << "%";
                std::cout << ss.str() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error in CrossExchangeSpotPerpStrategy::monitorPositions: " 
           << e.what();
        std::cerr << ss.str() << std::endl;
    }
}

std::set<std::string> CrossExchangeSpotPerpStrategy::getSymbols() const {
    std::set<std::string> symbols;
    
    try {
        auto spot_instruments = spot_exchange_->getAvailableInstruments(MarketType::SPOT);
        auto perp_instruments = perp_exchange_->getAvailableInstruments(MarketType::PERPETUAL);
        
        for (const auto& spot : spot_instruments) {
            symbols.insert(spot.symbol);
        }
        
        for (const auto& perp : perp_instruments) {
            symbols.insert(perp.symbol);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting symbols: " << e.what() << std::endl;
    }
    
    return symbols;
}

std::string CrossExchangeSpotPerpStrategy::getName() const {
    return "CrossExchangeSpotPerpStrategy (" + spot_exchange_->getName() + " spot & " + 
           perp_exchange_->getName() + " perp)";
}

} // namespace funding 