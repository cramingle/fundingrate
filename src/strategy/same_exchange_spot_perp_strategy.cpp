#include <strategy/arbitrage_strategy.h>
#include <exchange/exchange_interface.h>
#include <exchange/types.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

namespace funding {

SameExchangeSpotPerpStrategy::SameExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> exchange)
    : exchange_(exchange) {
    std::stringstream ss;
    ss << "Initialized with exchange: " << exchange_->getName();
    std::cout << ss.str() << std::endl;
}

std::vector<ArbitrageOpportunity> SameExchangeSpotPerpStrategy::findOpportunities() {
    std::vector<ArbitrageOpportunity> opportunities;
    
    try {
        // Get all available perpetual futures
        auto perp_instruments = exchange_->getAvailableInstruments(MarketType::PERPETUAL);
        if (perp_instruments.empty()) {
            return opportunities;
        }

        // Get all available spot instruments
        auto spot_instruments = exchange_->getAvailableInstruments(MarketType::SPOT);
        if (spot_instruments.empty()) {
            return opportunities;
        }
        
        // Find matching pairs and evaluate funding rates
        for (const auto& perp : perp_instruments) {
            // Find matching spot instrument with same base currency
            auto spot_it = std::find_if(spot_instruments.begin(), spot_instruments.end(),
                [&perp](const Instrument& spot) {
                    return spot.base_currency == perp.base_currency;
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
            
            // Skip if price spread is too large relative to funding
            if (std::abs(price_spread_pct) > max_spread) {
                continue;
            }
            
            // Calculate breakeven periods
            double periods_to_breakeven = total_transaction_cost_pct / std::abs(funding.rate * 100.0);
            
            // Get order book depth for liquidity assessment
            OrderBook spot_book = exchange_->getOrderBook(spot_it->symbol, 20);
            OrderBook perp_book = exchange_->getOrderBook(perp.symbol, 20);
            
            // Determine max position size based on liquidity
            double spot_liquidity = 0.0;
            double perp_liquidity = 0.0;
            
            if (funding.rate > 0) {
                // We'll go long spot, short perp - need sell liquidity for spot, buy for perp
                for (const auto& level : spot_book.asks) {
                    spot_liquidity += level.amount * level.price;
                }
                
                for (const auto& level : perp_book.bids) {
                    perp_liquidity += level.amount * level.price;
                }
            } else {
                // We'll go short spot, long perp - need buy liquidity for spot, sell for perp
                for (const auto& level : spot_book.bids) {
                    spot_liquidity += level.amount * level.price;
                }
                
                for (const auto& level : perp_book.asks) {
                    perp_liquidity += level.amount * level.price;
                }
            }
            
            // Take the smaller of the two liquidities and apply a conservative factor
            double max_position_size = std::min(spot_liquidity, perp_liquidity) * 0.1; // Use at most 10% of available liquidity
            
            // Calculate estimated profit
            double estimated_profit = std::abs(annualized_rate) - total_transaction_cost_pct;
            
            // Skip if not profitable
            if (estimated_profit <= 0) {
                continue;
            }
            
            // Create opportunity
            ArbitrageOpportunity opportunity;
            opportunity.pair = pair;
            opportunity.funding_rate1 = 0.0; // No funding for spot
            opportunity.funding_rate2 = funding.rate;
            opportunity.net_funding_rate = annualized_rate;
            opportunity.payment_interval1 = std::chrono::hours(0);
            opportunity.payment_interval2 = funding.payment_interval;
            opportunity.entry_price_spread = price_spread_pct;
            opportunity.max_allowable_spread = max_spread;
            opportunity.transaction_cost_pct = total_transaction_cost_pct;
            opportunity.estimated_profit = estimated_profit;
            opportunity.periods_to_breakeven = periods_to_breakeven;
            opportunity.max_position_size = max_position_size;
            opportunity.position_risk_score = calculateRiskScore(opportunity);
            opportunity.discovery_time = std::chrono::system_clock::now();
            
            opportunities.push_back(opportunity);
        }
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error finding opportunities: " << e.what();
        std::cerr << ss.str() << std::endl;
    }
    
    // Sort by estimated profit (descending)
    std::sort(opportunities.begin(), opportunities.end(),
              [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                  return a.estimated_profit > b.estimated_profit;
              });
    
    return opportunities;
}

bool SameExchangeSpotPerpStrategy::executeTrade(const ArbitrageOpportunity& opportunity, double size) {
    if (!validateOpportunity(opportunity)) {
        std::stringstream ss;
        ss << "Invalid opportunity for " << opportunity.pair.symbol1 << " / " << opportunity.pair.symbol2;
        std::cerr << ss.str() << std::endl;
        return false;
    }
    
    try {
        // Determine trade direction based on funding rate
        OrderSide spot_side, perp_side;
        double perp_size = size;
        double spot_size = size;
        
        // Get the current funding rate to confirm direction
        FundingRate funding = exchange_->getFundingRate(opportunity.pair.symbol2);
        
        if (funding.rate > 0) {
            // If funding is positive, go long spot and short perp
            spot_side = OrderSide::BUY;
            perp_side = OrderSide::SELL;
        } else {
            // If funding is negative, go short spot and long perp
            spot_side = OrderSide::SELL;
            perp_side = OrderSide::BUY;
        }
        
        std::stringstream logMsg;
        logMsg << "Executing trade for " << opportunity.pair.symbol1 << " / " << opportunity.pair.symbol2 
               << " with size " << size << " in " << exchange_->getName();
        std::cout << logMsg.str() << std::endl;
        
        // Execute spot order first (less slippage usually)
        Order spotOrder;
        spotOrder.symbol = opportunity.pair.symbol1;
        spotOrder.side = spot_side;
        spotOrder.type = OrderType::MARKET;
        spotOrder.amount = spot_size;
        spotOrder.price = 0; // Market order
        
        std::string spot_order_id = exchange_->placeOrder(spotOrder);
        
        if (spot_order_id.empty()) {
            std::stringstream ss;
            ss << "Failed to place spot order for " << opportunity.pair.symbol1;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Execute perp order
        Order perpOrder;
        perpOrder.symbol = opportunity.pair.symbol2;
        perpOrder.side = perp_side;
        perpOrder.type = OrderType::MARKET;
        perpOrder.amount = perp_size;
        perpOrder.price = 0; // Market order
        
        std::string perp_order_id = exchange_->placeOrder(perpOrder);
        
        if (perp_order_id.empty()) {
            // If perp order failed, try to close the spot position
            std::stringstream ss;
            ss << "Failed to place perp order for " << opportunity.pair.symbol2 << ". Attempting to close spot position.";
            std::cerr << ss.str() << std::endl;
            
            // Try to close the spot position
            Order reverseSpotOrder;
            reverseSpotOrder.symbol = opportunity.pair.symbol1;
            reverseSpotOrder.side = spot_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
            reverseSpotOrder.type = OrderType::MARKET;
            reverseSpotOrder.amount = spot_size;
            reverseSpotOrder.price = 0; // Market order
            
            exchange_->placeOrder(reverseSpotOrder);
            return false;
        }
        
        std::stringstream successMsg;
        successMsg << "Successfully executed trade for " << opportunity.pair.symbol1 << " / " << opportunity.pair.symbol2
                   << " (Spot Order ID: " << spot_order_id << ", Perp Order ID: " << perp_order_id << ")";
        std::cout << successMsg.str() << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error executing trade: " << e.what();
        std::cerr << ss.str() << std::endl;
        return false;
    }
}

bool SameExchangeSpotPerpStrategy::closePosition(const ArbitrageOpportunity& opportunity) {
    try {
        // Get current positions
        auto positions = exchange_->getOpenPositions();
        
        // Find matching perpetual position
        auto perp_pos = std::find_if(positions.begin(), positions.end(),
            [&opportunity](const Position& pos) {
                return pos.symbol == opportunity.pair.symbol2;
            });
        
        if (perp_pos == positions.end()) {
            std::stringstream ss;
            ss << "No open position found for " << opportunity.pair.symbol2;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Get balances to find spot position
        auto balances = exchange_->getAccountBalance();
        std::string base_currency = opportunity.pair.symbol1.substr(0, opportunity.pair.symbol1.find('/'));
        
        if (balances.total.find(base_currency) == balances.total.end()) {
            std::stringstream ss;
            ss << "No balance found for " << base_currency;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Determine position sizes
        double perp_size = std::abs(perp_pos->size);
        double spot_size = balances.total[base_currency];
        
        // Determine close directions
        OrderSide perp_close_side = perp_pos->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        OrderSide spot_close_side = perp_close_side == OrderSide::SELL ? OrderSide::SELL : OrderSide::BUY;
        
        // Close perp position
        Order perpCloseOrder;
        perpCloseOrder.symbol = opportunity.pair.symbol2;
        perpCloseOrder.side = perp_close_side;
        perpCloseOrder.type = OrderType::MARKET;
        perpCloseOrder.amount = perp_size;
        perpCloseOrder.price = 0; // Market order
        
        std::string perp_order_id = exchange_->placeOrder(perpCloseOrder);
        
        if (perp_order_id.empty()) {
            std::stringstream ss;
            ss << "Failed to close perp position for " << opportunity.pair.symbol2;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Close spot position
        Order spotCloseOrder;
        spotCloseOrder.symbol = opportunity.pair.symbol1;
        spotCloseOrder.side = spot_close_side;
        spotCloseOrder.type = OrderType::MARKET;
        spotCloseOrder.amount = spot_size;
        spotCloseOrder.price = 0; // Market order
        
        std::string spot_order_id = exchange_->placeOrder(spotCloseOrder);
        
        if (spot_order_id.empty()) {
            std::stringstream ss;
            ss << "Failed to close spot position for " << opportunity.pair.symbol1;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        std::stringstream ss;
        ss << "Successfully closed position for " << opportunity.pair.symbol1 << " / " << opportunity.pair.symbol2;
        std::cout << ss.str() << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error closing position: " << e.what();
        std::cerr << ss.str() << std::endl;
        return false;
    }
}

void SameExchangeSpotPerpStrategy::monitorPositions() {
    try {
        // Get all positions on the exchange
        auto perp_positions = exchange_->getOpenPositions();
        auto balances = exchange_->getAccountBalance();
        
        // For each active position
        for (const auto& perp_pos : perp_positions) {
            if (perp_pos.size == 0) continue;
            
            // Find the matching spot position (assuming the symbol format is BASE/QUOTE)
            std::string symbol = perp_pos.symbol;
            std::string base_currency = symbol.substr(0, symbol.find('/'));
            
            // Skip if we don't have a balance in this currency
            if (balances.total.find(base_currency) == balances.total.end() || 
                balances.total[base_currency] == 0) {
                continue;
            }
            
            // Get current funding rate
            FundingRate funding = exchange_->getFundingRate(symbol);
            
            // Log the current funding rate
            std::stringstream fundingMsg;
            fundingMsg << "Current funding rate for " << symbol << ": " << (funding.rate * 100) 
                      << "%, next payment in " << 
                      std::chrono::duration_cast<std::chrono::minutes>(
                          funding.next_payment - std::chrono::system_clock::now()).count() 
                      << " minutes";
            std::cout << fundingMsg.str() << std::endl;
            
            // Check if funding direction has flipped
            bool funding_flipped = false;
            
            if ((perp_pos.size > 0 && funding.rate < -0.0001) ||  // We're long but funding is now negative
                (perp_pos.size < 0 && funding.rate > 0.0001)) {   // We're short but funding is now positive
                funding_flipped = true;
            }
            
            // Get current prices
            double spot_price = exchange_->getPrice(symbol); // Same symbol for spot, assuming format is consistent
            double perp_price = exchange_->getPrice(symbol);
            
            // Calculate current spread
            double current_spread = (perp_price - spot_price) / spot_price * 100.0;
            
            // Position information log
            std::stringstream positionMsg;
            positionMsg << "Monitoring position: " << symbol 
                       << " Perp Size: " << perp_pos.size 
                       << " Base Currency Balance: " << balances.total[base_currency]
                       << " Current Spread: " << current_spread << "%";
            std::cout << positionMsg.str() << std::endl;
            
            // Decision making
            bool should_reduce = false;
            double reduction_factor = 0.5; // Reduce by 50% by default
            std::string reason;
            
            // If funding flipped, consider closing the position
            if (funding_flipped) {
                should_reduce = true;
                reason = "Funding rate direction changed";
            }
            
            // If spread has widened significantly, consider reducing position
            // Define "max_spread_tolerance" based on strategy parameters
            double max_spread_tolerance = 0.5; // 0.5% spread tolerance
            
            if (std::abs(current_spread) > max_spread_tolerance) {
                should_reduce = true;
                reason = "Price spread exceeded tolerance";
                
                // Adjust reduction factor based on how far the spread has widened
                reduction_factor = std::min(0.8, std::abs(current_spread) / max_spread_tolerance * 0.5);
            }
            
            // If we need to reduce position
            if (should_reduce) {
                // Calculate reduction sizes
                double perp_reduce_size = std::abs(perp_pos.size) * reduction_factor;
                double spot_reduce_size = std::min(
                    balances.total[base_currency] * reduction_factor,
                    balances.available[base_currency] // Can't reduce more than what's available
                );
                
                // Ensure we don't try to close more spot than we have available
                if (spot_reduce_size <= 0 || perp_reduce_size <= 0) {
                    std::stringstream ss;
                    ss << "Cannot reduce position for " << symbol << ": Insufficient balance";
                    std::cerr << ss.str() << std::endl;
                    continue;
                }
                
                // Determine close directions
                OrderSide perp_close_side = perp_pos.size > 0 ? OrderSide::SELL : OrderSide::BUY;
                OrderSide spot_close_side = perp_close_side == OrderSide::SELL ? OrderSide::SELL : OrderSide::BUY;
                
                std::stringstream reduceMsg;
                reduceMsg << "Reducing position for " << symbol << " by " << (reduction_factor * 100) 
                         << "% due to: " << reason;
                std::cout << reduceMsg.str() << std::endl;
                
                // Reduce perp position first
                Order perpReduceOrder;
                perpReduceOrder.symbol = symbol;
                perpReduceOrder.side = perp_close_side;
                perpReduceOrder.type = OrderType::MARKET;
                perpReduceOrder.amount = perp_reduce_size;
                perpReduceOrder.price = 0; // Market order
                
                std::string perp_order_id = exchange_->placeOrder(perpReduceOrder);
                
                if (perp_order_id.empty()) {
                    std::stringstream ss;
                    ss << "Failed to reduce perp position for " << symbol;
                    std::cerr << ss.str() << std::endl;
                    continue;
                }
                
                // Then reduce spot position
                Order spotReduceOrder;
                spotReduceOrder.symbol = symbol;
                spotReduceOrder.side = spot_close_side;
                spotReduceOrder.type = OrderType::MARKET;
                spotReduceOrder.amount = spot_reduce_size;
                spotReduceOrder.price = 0; // Market order
                
                std::string spot_order_id = exchange_->placeOrder(spotReduceOrder);
                
                if (spot_order_id.empty()) {
                    std::stringstream ss;
                    ss << "Failed to reduce spot position for " << symbol << ". Attempting to revert perp reduction.";
                    std::cerr << ss.str() << std::endl;
                    
                    // Try to revert the perp reduction to maintain hedge
                    Order revertPerpOrder;
                    revertPerpOrder.symbol = symbol;
                    revertPerpOrder.side = perp_close_side == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
                    revertPerpOrder.type = OrderType::MARKET;
                    revertPerpOrder.amount = perp_reduce_size;
                    revertPerpOrder.price = 0; // Market order
                    
                    exchange_->placeOrder(revertPerpOrder);
                    continue;
                }
                
                std::stringstream successMsg;
                successMsg << "Successfully reduced position for " << symbol;
                std::cout << successMsg.str() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error monitoring positions: " << e.what();
        std::cerr << ss.str() << std::endl;
    }
}

bool SameExchangeSpotPerpStrategy::validateOpportunity(const ArbitrageOpportunity& opportunity) {
    try {
        // Ensure the exchange names match
        if (opportunity.pair.exchange1 != exchange_->getName() || 
            opportunity.pair.exchange2 != exchange_->getName()) {
            return false;
        }
        
        // Ensure one is spot and one is perpetual
        if (opportunity.pair.market_type1 != MarketType::SPOT || 
            opportunity.pair.market_type2 != MarketType::PERPETUAL) {
            return false;
        }
        
        // Get current funding rate
        FundingRate funding = exchange_->getFundingRate(opportunity.pair.symbol2);
        
        // Validate that funding rate hasn't flipped direction
        if ((opportunity.funding_rate2 > 0 && funding.rate < 0) ||
            (opportunity.funding_rate2 < 0 && funding.rate > 0)) {
            std::stringstream ss;
            ss << "Funding rate direction flipped for " << opportunity.pair.symbol2 
               << " from " << opportunity.funding_rate2 << " to " << funding.rate;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Check that funding rate is still significant
        if (std::abs(funding.rate) < 0.0001) { // Same threshold as in findOpportunities
            std::stringstream ss;
            ss << "Funding rate too small for " << opportunity.pair.symbol2 
               << ": " << funding.rate;
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Get current prices and recalculate spread
        double spot_price = exchange_->getPrice(opportunity.pair.symbol1);
        double perp_price = exchange_->getPrice(opportunity.pair.symbol2);
        double current_spread = (perp_price - spot_price) / spot_price * 100.0;
        
        // Check that spread hasn't widened too much
        if (std::abs(current_spread) > opportunity.max_allowable_spread) {
            std::stringstream ss;
            ss << "Spread too wide for " << opportunity.pair.symbol1 << "/" << opportunity.pair.symbol2 
               << ": " << current_spread << "% > " << opportunity.max_allowable_spread << "%";
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        // Recalculate profitability
        double hours_per_year = 24.0 * 365.0;
        double payments_per_year = hours_per_year / funding.payment_interval.count();
        double annualized_rate = funding.rate * payments_per_year * 100.0;
        
        // Get current fees
        double spot_taker_fee = exchange_->getTradingFee(opportunity.pair.symbol1, false);
        double perp_taker_fee = exchange_->getTradingFee(opportunity.pair.symbol2, false);
        double current_transaction_cost = (spot_taker_fee + perp_taker_fee) * 2 * 100.0;
        
        // Calculate current expected profit
        double current_profit = std::abs(annualized_rate) - current_transaction_cost;
        
        // Check that opportunity is still profitable
        if (current_profit <= 0) {
            std::stringstream ss;
            ss << "Opportunity no longer profitable for " << opportunity.pair.symbol1 << "/" << opportunity.pair.symbol2 
               << ": " << current_profit << "%";
            std::cerr << ss.str() << std::endl;
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error validating opportunity: " << e.what();
        std::cerr << ss.str() << std::endl;
        return false;
    }
}

double SameExchangeSpotPerpStrategy::calculateOptimalPositionSize(const ArbitrageOpportunity& opportunity) {
    // Default to the maximum size from findOpportunities
    double size = opportunity.max_position_size;
    
    try {
        // Get account balance to check for available funds
        auto balance = exchange_->getAccountBalance();
        
        // Extract base currency from spot symbol (assuming format like BTC/USDT)
        std::string base_currency = opportunity.pair.symbol1.substr(0, opportunity.pair.symbol1.find('/'));
        std::string quote_currency = opportunity.pair.symbol1.substr(opportunity.pair.symbol1.find('/') + 1);
        
        // Calculate maximum position size based on available balance
        double available_base = 0.0;
        double available_quote = 0.0;
        
        if (balance.available.find(base_currency) != balance.available.end()) {
            available_base = balance.available[base_currency];
        }
        
        if (balance.available.find(quote_currency) != balance.available.end()) {
            available_quote = balance.available[quote_currency];
        }
        
        // Get current prices
        double spot_price = exchange_->getPrice(opportunity.pair.symbol1);
        
        // Calculate max size based on available balance
        double max_base_size = available_base;
        double max_quote_size = available_quote / spot_price;
        
        // For short positions, we're limited by available base currency
        // For long positions, we're limited by available quote currency
        FundingRate funding = exchange_->getFundingRate(opportunity.pair.symbol2);
        double balance_limited_size = 0.0;
        
        if (funding.rate > 0) {
            // We'll go long spot, short perp - limited by quote currency
            balance_limited_size = max_quote_size;
        } else {
            // We'll go short spot, long perp - limited by base currency
            balance_limited_size = max_base_size;
        }
        
        // Apply a safety factor to avoid using 100% of available balance
        balance_limited_size *= 0.95;
        
        // Take the minimum of the balance-limited size and the liquidity-limited size
        size = std::min(balance_limited_size, opportunity.max_position_size);
        
        // Apply risk-based scaling based on opportunity's risk score
        double risk_factor = 1.0 - (opportunity.position_risk_score / 100.0);
        size *= risk_factor;
        
    } catch (const std::exception& e) {
        std::stringstream ss;
        ss << "Error calculating position size: " << e.what();
        std::cerr << ss.str() << std::endl;
        // Fall back to default size with a safety factor
        size = opportunity.max_position_size * 0.5;
    }
    
    // Ensure we have a non-zero size
    return std::max(size, 0.0);
}

double SameExchangeSpotPerpStrategy::calculateRiskScore(const ArbitrageOpportunity& opportunity) {
    // Calculate a composite risk score (0-100) based on various factors
    double score = 0.0;
    
    // 1. Funding rate volatility risk (higher funding = higher risk)
    double funding_risk = std::min(std::abs(opportunity.funding_rate2) * 1000.0, 30.0);
    
    // 2. Spread risk (closer to max allowable = higher risk)
    double spread_percent = std::abs(opportunity.entry_price_spread) / opportunity.max_allowable_spread * 100.0;
    double spread_risk = std::min(spread_percent / 2.0, 30.0);
    
    // 3. Liquidity risk (inverse relationship with position size)
    double liquidity_risk = std::min(50000.0 / opportunity.max_position_size, 20.0);
    
    // 4. Exchange risk (fixed for this strategy since it's same exchange)
    double exchange_risk = 10.0;
    
    // Combine all risk factors
    score = funding_risk + spread_risk + liquidity_risk + exchange_risk;
    
    // Cap at 100
    return std::min(score, 100.0);
}

} // namespace funding