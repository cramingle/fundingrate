#include <strategy/arbitrage_strategy.h>
#include <exchange/exchange_interface.h>
#include <exchange/types.h>
#include <risk/risk_manager.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <set>

namespace funding {

SameExchangeSpotPerpStrategy::SameExchangeSpotPerpStrategy(std::shared_ptr<ExchangeInterface> exchange)
    : exchange_(exchange) {
    std::stringstream ss;
    ss << "Initialized with exchange: " << exchange_->getName();
    std::cout << ss.str() << std::endl;
    
    // We won't initialize the risk manager here since it's a forward declaration
    // The risk manager will be set by the strategy factory
}

std::set<std::string> SameExchangeSpotPerpStrategy::getSymbols() const {
    std::set<std::string> symbols;
    
    try {
        // Get all available perpetual futures
        auto perp_instruments = exchange_->getAvailableInstruments(MarketType::PERPETUAL);
        
        // Get all available spot instruments
        auto spot_instruments = exchange_->getAvailableInstruments(MarketType::SPOT);
        
        // Add all symbols to the set
        for (const auto& perp : perp_instruments) {
            symbols.insert(perp.symbol);
        }
        
        for (const auto& spot : spot_instruments) {
            symbols.insert(spot.symbol);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error in SameExchangeSpotPerpStrategy::getSymbols: " << e.what() << std::endl;
    }
    
    return symbols;
}

std::string SameExchangeSpotPerpStrategy::getName() const {
    return "SameExchangeSpotPerpStrategy (" + exchange_->getName() + ")";
}

std::vector<ArbitrageOpportunity> SameExchangeSpotPerpStrategy::findOpportunities() {
    std::vector<ArbitrageOpportunity> opportunities;
    
    try {
        // Get all available perpetual instruments
        std::cout << "Getting perpetual instruments from " << exchange_->getName() << std::endl;
        std::vector<Instrument> perp_instruments;
        try {
            perp_instruments = exchange_->getAvailableInstruments(MarketType::PERPETUAL);
            if (perp_instruments.empty()) {
                std::cout << "No perpetual instruments found on " << exchange_->getName() << std::endl;
                return opportunities;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching perpetual instruments from " << exchange_->getName() 
                      << ": " << e.what() << std::endl;
            return opportunities;
        }
        std::cout << "Found " << perp_instruments.size() << " perpetual instruments on " 
                  << exchange_->getName() << std::endl;

        // Get all available spot instruments
        std::cout << "Getting spot instruments from " << exchange_->getName() << std::endl;
        std::vector<Instrument> spot_instruments;
        try {
            spot_instruments = exchange_->getAvailableInstruments(MarketType::SPOT);
            if (spot_instruments.empty()) {
                std::cout << "No spot instruments found on " << exchange_->getName() << std::endl;
                return opportunities;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching spot instruments from " << exchange_->getName() 
                      << ": " << e.what() << std::endl;
            return opportunities;
        }
        std::cout << "Found " << spot_instruments.size() << " spot instruments on " 
                  << exchange_->getName() << std::endl;
        
        // Find matching pairs and evaluate funding rates
        for (const auto& perp : perp_instruments) {
            // Skip instruments with empty base or quote currency
            if (perp.base_currency.empty() || perp.quote_currency.empty()) {
                std::cout << "Skipping perpetual instrument with empty base or quote currency: " 
                          << perp.symbol << std::endl;
                continue;
            }
            
            // Find matching spot instrument
            std::string spot_symbol_prefix = perp.base_currency + "/" + perp.quote_currency;
            
            // Find all spot instruments that match the base/quote pair
            for (const auto& spot : spot_instruments) {
                // Skip if spot instrument doesn't match the perpetual's base/quote
                if (spot.symbol.find(spot_symbol_prefix) != 0) {
                    continue;
                }
                
                try {
                    // Get funding rate for the perpetual
                    FundingRate funding;
                    try {
                        funding = exchange_->getFundingRate(perp.symbol);
                    } catch (const std::exception& e) {
                        std::cerr << "Error fetching funding rate for " << perp.symbol 
                                  << ": " << e.what() << std::endl;
                        continue;
                    }
                    
                    // Skip if funding rate is too small
                    if (std::abs(funding.rate) < min_funding_rate_) {
                        continue;
                    }
                    
                    // Calculate annualized funding rate
                    double hours_per_year = 24.0 * 365.0;
                    double payments_per_year = hours_per_year / funding.payment_interval.count();
                    double annualized_rate = funding.rate * payments_per_year * 100.0;
                    
                    std::cout << "Annualized funding rate for " << perp.symbol << ": " 
                              << annualized_rate << "%" << std::endl;
                    
                    // Get current prices
                    double spot_price = 0.0;
                    double perp_price = 0.0;
                    
                    try {
                        spot_price = exchange_->getPrice(spot.symbol);
                    } catch (const std::exception& e) {
                        std::cerr << "Error fetching price for " << spot.symbol 
                                  << ": " << e.what() << std::endl;
                        continue;
                    }
                    
                    try {
                        perp_price = exchange_->getPrice(perp.symbol);
                    } catch (const std::exception& e) {
                        std::cerr << "Error fetching price for " << perp.symbol 
                                  << ": " << e.what() << std::endl;
                        continue;
                    }
                    
                    // Skip if either price is invalid
                    if (spot_price <= 0.0 || perp_price <= 0.0) {
                        std::cerr << "Invalid prices for " << spot.symbol << " (" << spot_price 
                                  << ") or " << perp.symbol << " (" << perp_price << ")" << std::endl;
                        continue;
                    }
                    
                    // Calculate price spread as a percentage
                    double price_spread_pct = (perp_price - spot_price) / spot_price * 100.0;
                    
                    std::cout << "Price spread for " << spot.symbol << " vs " << perp.symbol 
                              << ": " << price_spread_pct << "%" << std::endl;
                    
                    // Get trading fees
                    double spot_taker_fee = 0.0;
                    double perp_taker_fee = 0.0;
                    
                    try {
                        spot_taker_fee = exchange_->getTradingFee(spot.symbol, false);
                    } catch (const std::exception& e) {
                        std::cerr << "Error fetching trading fee for " << spot.symbol 
                                  << ": " << e.what() << std::endl;
                        // Use default fee as fallback
                        spot_taker_fee = 0.001;
                    }
                    
                    try {
                        perp_taker_fee = exchange_->getTradingFee(perp.symbol, false);
                    } catch (const std::exception& e) {
                        std::cerr << "Error fetching trading fee for " << perp.symbol 
                                  << ": " << e.what() << std::endl;
                        // Use default fee as fallback
                        perp_taker_fee = 0.001;
                    }
                    
                    // Calculate transaction costs (entry + exit)
                    double transaction_cost = (spot_taker_fee + perp_taker_fee) * 2 * 100.0;
                    
                    // Calculate estimated profit (annualized funding rate minus transaction costs)
                    double estimated_profit = std::abs(annualized_rate) - transaction_cost;
                    
                    // Skip if estimated profit is below threshold
                    if (estimated_profit < min_expected_profit_) {
                        continue;
                    }
                    
                    // Calculate maximum allowable spread based on funding rate
                    double max_allowable_spread = std::abs(annualized_rate) * 0.1; // 10% of annualized rate
                    
                    // Skip if current spread is too wide
                    if (std::abs(price_spread_pct) > max_allowable_spread) {
                        std::cout << "Spread too wide for " << spot.symbol << " vs " << perp.symbol 
                                  << ": " << price_spread_pct << "% > " << max_allowable_spread << "%" << std::endl;
                        continue;
                    }
                    
                    // Calculate liquidity risk
                    double liquidity_risk = 0.0;
                    try {
                        OrderBook spot_book = exchange_->getOrderBook(spot.symbol, 5);
                        OrderBook perp_book = exchange_->getOrderBook(perp.symbol, 5);
                        
                        // Calculate average bid-ask spread as a percentage
                        double spot_spread = (spot_book.asks[0].price - spot_book.bids[0].price) / spot_book.bids[0].price;
                        double perp_spread = (perp_book.asks[0].price - perp_book.bids[0].price) / perp_book.bids[0].price;
                        
                        // Calculate total available liquidity
                        double spot_liquidity = 0.0;
                        double perp_liquidity = 0.0;
                        
                        for (const auto& level : spot_book.bids) {
                            spot_liquidity += level.amount * level.price;
                        }
                        
                        for (const auto& level : perp_book.bids) {
                            perp_liquidity += level.amount * level.price;
                        }
                        
                        // Higher spread and lower liquidity = higher risk
                        liquidity_risk = (spot_spread + perp_spread) * 5000.0 + 
                                        (1000000.0 / (spot_liquidity + 1.0)) + 
                                        (1000000.0 / (perp_liquidity + 1.0));
                        
                        // Cap liquidity risk at 50
                        liquidity_risk = std::min(50.0, liquidity_risk);
                    } catch (const std::exception& e) {
                        std::cerr << "Error calculating liquidity risk: " << e.what() << std::endl;
                        liquidity_risk = 25.0; // Default to medium risk if calculation fails
                    }
                    
                    // Create opportunity object
                    ArbitrageOpportunity opportunity;
                    opportunity.pair.exchange1 = exchange_->getName();
                    opportunity.pair.symbol1 = spot.symbol;
                    opportunity.pair.market_type1 = MarketType::SPOT;
                    opportunity.pair.exchange2 = exchange_->getName();
                    opportunity.pair.symbol2 = perp.symbol;
                    opportunity.pair.market_type2 = MarketType::PERPETUAL;
                    opportunity.funding_rate1 = 0.0; // Spot doesn't pay funding
                    opportunity.funding_rate2 = funding.rate;
                    opportunity.net_funding_rate = annualized_rate;
                    opportunity.payment_interval1 = std::chrono::hours(0);
                    opportunity.payment_interval2 = funding.payment_interval;
                    opportunity.entry_price_spread = price_spread_pct;
                    opportunity.max_allowable_spread = max_allowable_spread;
                    opportunity.transaction_cost_pct = transaction_cost;
                    opportunity.estimated_profit = estimated_profit;
                    opportunity.periods_to_breakeven = transaction_cost / std::abs(funding.rate * 100.0);
                    opportunity.discovery_time = std::chrono::system_clock::now();
                    opportunity.strategy_type = "SameExchangeSpotPerpStrategy";
                    opportunity.strategy_index = -1; // Will be set by CompositeStrategy if used
                    
                    // Calculate position risk score (0-100, higher = riskier)
                    double funding_risk = 10.0 * (1.0 - std::min(1.0, std::abs(funding.rate) / 0.01));
                    double spread_risk = 20.0 * (std::abs(price_spread_pct) / max_allowable_spread);
                    double exchange_risk = 10.0; // Base risk for any exchange
                    
                    opportunity.position_risk_score = std::min(100.0, spread_risk + liquidity_risk + exchange_risk + funding_risk);
                    
                    // Calculate maximum position size based on risk
                    double risk_factor = 1.0 - (opportunity.position_risk_score / 200.0); // 0.5 to 1.0
                    opportunity.max_position_size = 5000.0 * risk_factor; // Use a default max position size
                    
                    // Add to opportunities
                    opportunities.push_back(opportunity);
                    
                    std::cout << "Found opportunity: " << spot.symbol << " vs " << perp.symbol 
                              << " | Funding rate: " << (funding.rate * 100.0) << "%" 
                              << " | Est. profit: " << estimated_profit << "%" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "Error processing pair " << spot.symbol << " / " << perp.symbol 
                              << ": " << e.what() << std::endl;
                }
            }
        }
        
        // Sort opportunities by estimated profit
        std::sort(opportunities.begin(), opportunities.end(),
                 [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                     return a.estimated_profit > b.estimated_profit;
                 });
        
        std::cout << "Found " << opportunities.size() << " viable opportunities" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error in SameExchangeSpotPerpStrategy::findOpportunities: " << e.what() << std::endl;
    }
    
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