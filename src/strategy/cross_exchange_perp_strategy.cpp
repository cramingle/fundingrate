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
#include <iomanip>
#include <mutex>
#include <atomic>
#include <random>

namespace funding {

// Constants for retry logic and risk management
constexpr int MAX_API_RETRIES = 3;
constexpr int RETRY_DELAY_MS = 500;
constexpr double LIQUIDITY_IMPACT_THRESHOLD = 0.1; // 10% of available liquidity
constexpr double PRICE_SLIPPAGE_BUFFER = 0.0015; // 0.15% buffer for slippage
constexpr double EMERGENCY_SPREAD_THRESHOLD = 1.5; // 1.5% emergency threshold

// Thread-safe logging function
void logMessage(const std::string& strategy_name, const std::string& message, bool is_error = false) {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
        
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << now_ms.count();
    
    if (is_error) {
        std::cerr << "[" << ss.str() << "][" << strategy_name << "][ERROR] " << message << std::endl;
    } else {
        std::cout << "[" << ss.str() << "][" << strategy_name << "] " << message << std::endl;
    }
}

// API call with retry logic
template<typename Func>
auto retryApiCall(const std::string& operation, Func&& func, int max_retries = MAX_API_RETRIES) 
    -> decltype(func()) {
    
    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        try {
            return func();
        } catch (const std::exception& e) {
            std::stringstream ss;
            ss << operation << " failed (attempt " << attempt << "/" << max_retries 
               << "): " << e.what();
               
            if (attempt < max_retries) {
                ss << " - retrying in " << RETRY_DELAY_MS << "ms";
                logMessage("CrossExchangePerpStrategy", ss.str(), true);
                std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
            } else {
                ss << " - all retries exhausted";
                logMessage("CrossExchangePerpStrategy", ss.str(), true);
                throw; // Rethrow after max retries
            }
        }
    }
    
    // This should never be reached due to the throw in the catch block
    throw std::runtime_error("Unexpected error in retryApiCall");
}

CrossExchangePerpStrategy::CrossExchangePerpStrategy(std::shared_ptr<ExchangeInterface> exchange1,
                                                   std::shared_ptr<ExchangeInterface> exchange2)
    : exchange1_(exchange1), exchange2_(exchange2) {
    
    // Log strategy initialization
    std::stringstream ss;
    ss << "Initialized with exchanges: " << exchange1_->getName() 
       << " and " << exchange2_->getName();
    logMessage("CrossExchangePerpStrategy", ss.str());
}

std::set<std::string> CrossExchangePerpStrategy::getSymbols() const {
    std::set<std::string> symbols;
    
    try {
        // Get all available perpetual futures from both exchanges
        auto perp_instruments1 = exchange1_->getAvailableInstruments(MarketType::PERPETUAL);
        auto perp_instruments2 = exchange2_->getAvailableInstruments(MarketType::PERPETUAL);
        
        // Add symbols from both exchanges
        for (const auto& instrument : perp_instruments1) {
            symbols.insert(instrument.symbol);
        }
        for (const auto& instrument : perp_instruments2) {
            symbols.insert(instrument.symbol);
        }
        
    } catch (const std::exception& e) {
        logMessage("CrossExchangePerpStrategy", 
                  "Error getting available symbols: " + std::string(e.what()), true);
    }
    
    return symbols;
}

std::vector<ArbitrageOpportunity> CrossExchangePerpStrategy::findOpportunities() {
    std::vector<ArbitrageOpportunity> opportunities;
    
    try {
        // Get all available perpetual futures from both exchanges with retry logic
        auto perp_instruments1 = retryApiCall("Get instruments from " + exchange1_->getName(),
            [this]() { return exchange1_->getAvailableInstruments(MarketType::PERPETUAL); });
            
        auto perp_instruments2 = retryApiCall("Get instruments from " + exchange2_->getName(),
            [this]() { return exchange2_->getAvailableInstruments(MarketType::PERPETUAL); });
        
        logMessage("CrossExchangePerpStrategy", "Found " + std::to_string(perp_instruments1.size()) + 
                  " perpetuals on " + exchange1_->getName() + " and " + 
                  std::to_string(perp_instruments2.size()) + " on " + exchange2_->getName());
        
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
            
            // Get funding rates for both perpetuals with retry logic
            FundingRate funding1 = retryApiCall("Get funding rate for " + perp1.symbol,
                [this, &perp1]() { return exchange1_->getFundingRate(perp1.symbol); });
                
            FundingRate funding2 = retryApiCall("Get funding rate for " + perp2_it->symbol,
                [this, &perp2_it]() { return exchange2_->getFundingRate(perp2_it->symbol); });
            
            // Calculate funding rate differential
            double funding_diff = funding1.rate - funding2.rate;
            
            // Skip if funding rate differential is too small
            constexpr double MIN_FUNDING_DIFF = 0.0002; // 0.02% threshold - configurable
            if (std::abs(funding_diff) < MIN_FUNDING_DIFF) {
                continue;
            }
            
            // Get prices and calculate spread with retry logic
            double price1 = retryApiCall("Get price for " + perp1.symbol,
                [this, &perp1]() { return exchange1_->getPrice(perp1.symbol); });
                
            double price2 = retryApiCall("Get price for " + perp2_it->symbol,
                [this, &perp2_it]() { return exchange2_->getPrice(perp2_it->symbol); });
                
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
            
            // Get transaction fees for both exchanges with retry logic
            double taker_fee1 = retryApiCall("Get trading fee for " + perp1.symbol,
                [this, &perp1]() { return exchange1_->getTradingFee(perp1.symbol, false); });
                
            double taker_fee2 = retryApiCall("Get trading fee for " + perp2_it->symbol,
                [this, &perp2_it]() { return exchange2_->getTradingFee(perp2_it->symbol, false); });
            
            // Calculate total transaction cost for the complete arbitrage (entry + exit)
            // For each exchange: entry fee + exit fee (as percentage)
            double total_transaction_cost_pct = (taker_fee1 * 2 + taker_fee2 * 2) * 100.0;
            
            // Calculate max allowable spread before it negates funding
            // Use the more conservative payment frequency for calculations
            double min_payments = std::min(payments_per_year1, payments_per_year2);
            double max_spread = std::abs(net_funding_rate) / min_payments * 0.75;
            
            // Add slippage buffer to transaction costs based on market conditions
            double slippage_buffer = PRICE_SLIPPAGE_BUFFER * 100.0; // Convert to percentage
            total_transaction_cost_pct += slippage_buffer;
            
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
            
            // Get order books to estimate max position size with retry logic
            OrderBook ob1 = retryApiCall("Get order book for " + perp1.symbol,
                [this, &perp1]() { return exchange1_->getOrderBook(perp1.symbol, 10); });
                
            OrderBook ob2 = retryApiCall("Get order book for " + perp2_it->symbol,
                [this, &perp2_it]() { return exchange2_->getOrderBook(perp2_it->symbol, 10); });
            
            double liquidity1 = 0.0;
            double liquidity2 = 0.0;
            
            // Calculate available liquidity with market impact consideration
            bool long_exchange1 = funding1.rate < funding2.rate;
            
            if (long_exchange1) {
                // Market impact analysis - calculate effective price after slippage
                double effective_price1 = 0.0;
                double total_size1 = 0.0;
                double target_value = 50000.0; // Target position value in USD
                
                for (const auto& level : ob1.asks) {
                    double level_value = level.amount * level.price;
                    if (total_size1 + level.amount < target_value / level.price) {
                        // We can take the entire level
                        liquidity1 += level_value;
                        effective_price1 += level_value;
                        total_size1 += level.amount;
                    } else {
                        // We need part of this level
                        double needed_amount = (target_value / level.price) - total_size1;
                        liquidity1 += needed_amount * level.price;
                        effective_price1 += needed_amount * level.price;
                        total_size1 += needed_amount;
                        break;
                    }
                }
                
                if (total_size1 > 0) {
                    effective_price1 /= (total_size1 * price1); // Normalize to get price multiplier
                } else {
                    effective_price1 = 1.0; // No slippage if no size
                }
                
                // Repeat for exchange 2
                double effective_price2 = 0.0;
                double total_size2 = 0.0;
                
                for (const auto& level : ob2.bids) {
                    double level_value = level.amount * level.price;
                    if (total_size2 + level.amount < target_value / level.price) {
                        liquidity2 += level_value;
                        effective_price2 += level_value;
                        total_size2 += level.amount;
                    } else {
                        double needed_amount = (target_value / level.price) - total_size2;
                        liquidity2 += needed_amount * level.price;
                        effective_price2 += needed_amount * level.price;
                        total_size2 += needed_amount;
                        break;
                    }
                }
                
                if (total_size2 > 0) {
                    effective_price2 /= (total_size2 * price2);
                } else {
                    effective_price2 = 1.0;
                }
                
                // Calculate slippage as percentage of original price
                double slippage1 = (effective_price1 - 1.0) * 100.0;
                double slippage2 = (1.0 - effective_price2) * 100.0;
                double total_slippage = slippage1 + slippage2;
                
                // Add slippage to transaction costs if it wasn't already accounted for
                if (total_slippage > slippage_buffer) {
                    opportunity.transaction_cost_pct += (total_slippage - slippage_buffer);
                }
            } else {
                // Repeat market impact analysis for opposite direction
                // Short on exchange 1, long on exchange 2
                double effective_price1 = 0.0;
                double total_size1 = 0.0;
                double target_value = 50000.0;
                
                for (const auto& level : ob1.bids) {
                    double level_value = level.amount * level.price;
                    if (total_size1 + level.amount < target_value / level.price) {
                        liquidity1 += level_value;
                        effective_price1 += level_value;
                        total_size1 += level.amount;
                    } else {
                        double needed_amount = (target_value / level.price) - total_size1;
                        liquidity1 += needed_amount * level.price;
                        effective_price1 += needed_amount * level.price;
                        total_size1 += needed_amount;
                        break;
                    }
                }
                
                if (total_size1 > 0) {
                    effective_price1 /= (total_size1 * price1);
                } else {
                    effective_price1 = 1.0;
                }
                
                double effective_price2 = 0.0;
                double total_size2 = 0.0;
                
                for (const auto& level : ob2.asks) {
                    double level_value = level.amount * level.price;
                    if (total_size2 + level.amount < target_value / level.price) {
                        liquidity2 += level_value;
                        effective_price2 += level_value;
                        total_size2 += level.amount;
                    } else {
                        double needed_amount = (target_value / level.price) - total_size2;
                        liquidity2 += needed_amount * level.price;
                        effective_price2 += needed_amount * level.price;
                        total_size2 += needed_amount;
                        break;
                    }
                }
                
                if (total_size2 > 0) {
                    effective_price2 /= (total_size2 * price2);
                } else {
                    effective_price2 = 1.0;
                }
                
                double slippage1 = (1.0 - effective_price1) * 100.0;
                double slippage2 = (effective_price2 - 1.0) * 100.0;
                double total_slippage = slippage1 + slippage2;
                
                if (total_slippage > slippage_buffer) {
                    opportunity.transaction_cost_pct += (total_slippage - slippage_buffer);
                }
            }
            
            // Use smaller of the two for max position size and consider market impact
            double max_liquidity = std::min(liquidity1, liquidity2);
            opportunity.max_position_size = max_liquidity * 0.25; // 25% of available liquidity
            
            // Calculate a risk score that considers market conditions
            double spread_risk = std::abs(price_spread_pct / max_spread) * 40.0; // 40% of score
            double liquidity_risk = (1.0 - max_liquidity / 50000.0) * 30.0; // 30% of score
            
            // Dynamic exchange risk based on historical reliability
            double exchange1_risk = calculateExchangeRisk(exchange1_->getName());
            double exchange2_risk = calculateExchangeRisk(exchange2_->getName());
            double exchange_risk = (exchange1_risk + exchange2_risk) / 2.0;
            
            // Funding payment mismatch risk
            double payment_mismatch_risk = 0.0;
            if (funding1.payment_interval != funding2.payment_interval) {
                payment_mismatch_risk = 10.0; // Additional risk for mismatched payment intervals
            }
            
            double funding_risk = 10.0 + payment_mismatch_risk; // Base risk + mismatch risk
            
            opportunity.position_risk_score = std::min(100.0, spread_risk + liquidity_risk + exchange_risk + funding_risk);
            
            // Record discovery time
            opportunity.discovery_time = std::chrono::system_clock::now();
            
            // Set the strategy type
            opportunity.strategy_type = "CrossExchangePerpStrategy";
            opportunity.strategy_index = -1; // Will be set by CompositeStrategy if used
            
            // Add to opportunities if estimated profit is positive AFTER transaction costs
            // and the risk score is acceptable
            if (opportunity.estimated_profit > 0 && opportunity.position_risk_score < 75.0) {
                opportunities.push_back(opportunity);
                
                // Log identified opportunity
                std::stringstream ss;
                ss << "Found opportunity: " << perp1.symbol << " on " << exchange1_->getName()
                   << " vs " << perp2_it->symbol << " on " << exchange2_->getName()
                   << " | Funding diff: " << (funding_diff * 100.0) << "%"
                   << " | Est. profit: " << opportunity.estimated_profit << "%"
                   << " | Risk score: " << opportunity.position_risk_score;
                logMessage("CrossExchangePerpStrategy", ss.str());
            }
        }
    } catch (const std::exception& e) {
        logMessage("CrossExchangePerpStrategy", 
                 "Error in findOpportunities: " + std::string(e.what()), true);
    }
    
    // Sort opportunities by risk-adjusted return rather than just profit
    std::sort(opportunities.begin(), opportunities.end(),
             [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                 // Calculate risk-adjusted return (profit divided by risk)
                 double a_risk_adj = a.estimated_profit / (a.position_risk_score + 1.0);
                 double b_risk_adj = b.estimated_profit / (b.position_risk_score + 1.0);
                 return a_risk_adj > b_risk_adj;
             });
    
    logMessage("CrossExchangePerpStrategy", "Found " + 
               std::to_string(opportunities.size()) + " viable opportunities");
    
    return opportunities;
}

// Calculate exchange risk based on historical reliability
double CrossExchangePerpStrategy::calculateExchangeRisk(const std::string& exchange_name) {
    if (exchange_name == "Binance" || exchange_name == "Bybit") {
        return 5.0; // Lower risk for major exchanges
    } else if (exchange_name == "Bitget" || exchange_name == "OKX") {
        return 10.0; // Medium risk
    } else {
        return 15.0; // Higher risk for less established exchanges
    }
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
    logMessage("CrossExchangePerpStrategy", "Executing trade with size: " + 
              std::to_string(size) + " for pair: " + opportunity.pair.symbol1 + "/" + 
              opportunity.pair.symbol2);
    
    try {
        // Derive the order size for each leg with precision rounding
        double size1 = size;
        double size2 = size;
        
        // Re-validate the opportunity before execution
        if (!validateOpportunity(opportunity)) {
            logMessage("CrossExchangePerpStrategy", "Opportunity is no longer valid, aborting trade", true);
            return false;
        }
        
        // Get current prices with retry logic
        double price1 = retryApiCall("Get execution price for " + opportunity.pair.symbol1,
            [this, &opportunity]() { return exchange1_->getPrice(opportunity.pair.symbol1); });
            
        double price2 = retryApiCall("Get execution price for " + opportunity.pair.symbol2,
            [this, &opportunity]() { return exchange2_->getPrice(opportunity.pair.symbol2); });
        
        // Calculate spread as percentage
        double spread = std::abs(price1 - price2) / std::min(price1, price2) * 100.0;
        
        // Log current spread
        std::stringstream ss;
        ss << "Current spread: " << spread << "%, max allowable: " << opportunity.max_allowable_spread << "%";
        logMessage("CrossExchangePerpStrategy", ss.str());
        
        // If spread exceeds the threshold from opportunity, abort
        if (spread > opportunity.max_allowable_spread) {
            logMessage("CrossExchangePerpStrategy", "Spread too large: " + std::to_string(spread) + 
                      "%, max allowable: " + std::to_string(opportunity.max_allowable_spread) + 
                      "%, aborting trade", true);
            return false;
        }
        
        // Get order books to check for sufficient liquidity
        OrderBook ob1 = retryApiCall("Get execution order book for " + opportunity.pair.symbol1,
            [this, &opportunity]() { 
                return exchange1_->getOrderBook(opportunity.pair.symbol1, 5); 
            });
            
        OrderBook ob2 = retryApiCall("Get execution order book for " + opportunity.pair.symbol2,
            [this, &opportunity]() { 
                return exchange2_->getOrderBook(opportunity.pair.symbol2, 5); 
            });
        
        // Determine order sides based on funding rates, not just price difference
        // Typically we want to go long on the lower funding rate and short on the higher
        OrderSide side1, side2;
        if (opportunity.funding_rate1 < opportunity.funding_rate2) {
            // Long on exchange1, short on exchange2
            side1 = OrderSide::BUY;
            side2 = OrderSide::SELL;
            logMessage("CrossExchangePerpStrategy", "Direction: Long " + opportunity.pair.symbol1 + 
                      " on " + opportunity.pair.exchange1 + ", Short " + opportunity.pair.symbol2 + 
                      " on " + opportunity.pair.exchange2);
        } else {
            // Short on exchange1, long on exchange2
            side1 = OrderSide::SELL;
            side2 = OrderSide::BUY;
            logMessage("CrossExchangePerpStrategy", "Direction: Short " + opportunity.pair.symbol1 + 
                      " on " + opportunity.pair.exchange1 + ", Long " + opportunity.pair.symbol2 + 
                      " on " + opportunity.pair.exchange2);
        }
        
        // Check for sufficient liquidity on both sides
        double required_liquidity1 = size1 * price1;
        double required_liquidity2 = size2 * price2;
        
        double available_liquidity1 = 0.0;
        double available_liquidity2 = 0.0;
        
        // Calculate available liquidity based on order sides
        if (side1 == OrderSide::BUY) {
            for (const auto& level : ob1.asks) {
                available_liquidity1 += level.amount * level.price;
                if (available_liquidity1 >= required_liquidity1 * 1.5) break; // 50% buffer
            }
        } else {
            for (const auto& level : ob1.bids) {
                available_liquidity1 += level.amount * level.price;
                if (available_liquidity1 >= required_liquidity1 * 1.5) break; // 50% buffer
            }
        }
        
        if (side2 == OrderSide::BUY) {
            for (const auto& level : ob2.asks) {
                available_liquidity2 += level.amount * level.price;
                if (available_liquidity2 >= required_liquidity2 * 1.5) break; // 50% buffer
            }
        } else {
            for (const auto& level : ob2.bids) {
                available_liquidity2 += level.amount * level.price;
                if (available_liquidity2 >= required_liquidity2 * 1.5) break; // 50% buffer
            }
        }
        
        // Adjust order size if liquidity is insufficient
        if (available_liquidity1 < required_liquidity1 * 1.5) {
            double new_size1 = (available_liquidity1 / price1) * 0.6; // 60% of available liquidity
            logMessage("CrossExchangePerpStrategy", "Insufficient liquidity on " + 
                      opportunity.pair.exchange1 + ", reducing order size from " + 
                      std::to_string(size1) + " to " + std::to_string(new_size1));
            size1 = new_size1;
        }
        
        if (available_liquidity2 < required_liquidity2 * 1.5) {
            double new_size2 = (available_liquidity2 / price2) * 0.6; // 60% of available liquidity
            logMessage("CrossExchangePerpStrategy", "Insufficient liquidity on " + 
                      opportunity.pair.exchange2 + ", reducing order size from " + 
                      std::to_string(size2) + " to " + std::to_string(new_size2));
            size2 = new_size2;
        }
        
        // Use the smaller of the two sizes for both exchanges to maintain balance
        double adjusted_size = std::min(size1, size2);
        logMessage("CrossExchangePerpStrategy", "Final execution size: " + std::to_string(adjusted_size));
        
        std::string order1_id, order2_id;
        
        // Determine which exchange has better liquidity and execute on less liquid first
        // This is a common practice to reduce execution risk
        bool exchange1_first = available_liquidity1 <= available_liquidity2;
        
        // Create the order objects
        Order order1;
        order1.symbol = opportunity.pair.symbol1;
        order1.side = side1;
        order1.type = OrderType::MARKET;
        order1.amount = adjusted_size;
        
        Order order2;
        order2.symbol = opportunity.pair.symbol2;
        order2.side = side2;
        order2.type = OrderType::MARKET;
        order2.amount = adjusted_size;
        
        // Check account balances and margin requirements before placing orders
        bool can_execute = checkMarginRequirements(order1, order2);
        if (!can_execute) {
            logMessage("CrossExchangePerpStrategy", "Insufficient margin for execution", true);
            return false;
        }
        
        // Execute orders with sequential approach and careful verification
        if (exchange1_first) {
            // Place order on exchange 1 first
            logMessage("CrossExchangePerpStrategy", "Executing on " + opportunity.pair.exchange1 + " first");
            
            order1_id = retryApiCall("Place order on " + opportunity.pair.exchange1,
                [this, &order1]() { return exchange1_->placeOrder(order1); });
            
            if (order1_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to place order on " + 
                          opportunity.pair.exchange1, true);
                return false;
            }
            
            // Wait for execution and verify status
            bool order1_filled = waitForOrderFill(exchange1_, order1_id, 3);
            if (!order1_filled) {
                logMessage("CrossExchangePerpStrategy", "Order on " + opportunity.pair.exchange1 + 
                          " failed to fill, cancelling", true);
                exchange1_->cancelOrder(order1_id);
                return false;
            }
            
            // Now place order on exchange 2
            order2_id = retryApiCall("Place order on " + opportunity.pair.exchange2,
                [this, &order2]() { return exchange2_->placeOrder(order2); });
            
            if (order2_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to place order on " + 
                          opportunity.pair.exchange2 + ", attempting to close first position", true);
                
                // If second order fails, close the first position to avoid exposed risk
                Order closeOrder1;
                closeOrder1.symbol = opportunity.pair.symbol1;
                closeOrder1.side = side1 == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
                closeOrder1.type = OrderType::MARKET;
                closeOrder1.amount = adjusted_size;
                
                std::string close_id = exchange1_->placeOrder(closeOrder1);
                if (close_id.empty()) {
                    logMessage("CrossExchangePerpStrategy", "CRITICAL ERROR: Failed to close position on " + 
                              opportunity.pair.exchange1 + ", MANUAL INTERVENTION REQUIRED", true);
                }
                return false;
            }
            
            bool order2_filled = waitForOrderFill(exchange2_, order2_id, 3);
            if (!order2_filled) {
                logMessage("CrossExchangePerpStrategy", "Order on " + opportunity.pair.exchange2 + 
                          " failed to fill, attempting to close first position", true);
                
                exchange2_->cancelOrder(order2_id);
                
                // Close the first position
                Order closeOrder1;
                closeOrder1.symbol = opportunity.pair.symbol1;
                closeOrder1.side = side1 == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
                closeOrder1.type = OrderType::MARKET;
                closeOrder1.amount = adjusted_size;
                
                std::string close_id = exchange1_->placeOrder(closeOrder1);
                if (close_id.empty()) {
                    logMessage("CrossExchangePerpStrategy", "CRITICAL ERROR: Failed to close position on " + 
                              opportunity.pair.exchange1 + ", MANUAL INTERVENTION REQUIRED", true);
                }
                return false;
            }
        } else {
            // Place order on exchange 2 first
            logMessage("CrossExchangePerpStrategy", "Executing on " + opportunity.pair.exchange2 + " first");
            
            order2_id = retryApiCall("Place order on " + opportunity.pair.exchange2,
                [this, &order2]() { return exchange2_->placeOrder(order2); });
            
            if (order2_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to place order on " + 
                          opportunity.pair.exchange2, true);
                return false;
            }
            
            // Wait for execution and verify status
            bool order2_filled = waitForOrderFill(exchange2_, order2_id, 3);
            if (!order2_filled) {
                logMessage("CrossExchangePerpStrategy", "Order on " + opportunity.pair.exchange2 + 
                          " failed to fill, cancelling", true);
                exchange2_->cancelOrder(order2_id);
                return false;
            }
            
            // Now place order on exchange 1
            order1_id = retryApiCall("Place order on " + opportunity.pair.exchange1,
                [this, &order1]() { return exchange1_->placeOrder(order1); });
            
            if (order1_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to place order on " + 
                          opportunity.pair.exchange1 + ", attempting to close second position", true);
                
                // If first order fails, close the second position to avoid exposed risk
                Order closeOrder2;
                closeOrder2.symbol = opportunity.pair.symbol2;
                closeOrder2.side = side2 == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
                closeOrder2.type = OrderType::MARKET;
                closeOrder2.amount = adjusted_size;
                
                std::string close_id = exchange2_->placeOrder(closeOrder2);
                if (close_id.empty()) {
                    logMessage("CrossExchangePerpStrategy", "CRITICAL ERROR: Failed to close position on " + 
                              opportunity.pair.exchange2 + ", MANUAL INTERVENTION REQUIRED", true);
                }
                return false;
            }
            
            bool order1_filled = waitForOrderFill(exchange1_, order1_id, 3);
            if (!order1_filled) {
                logMessage("CrossExchangePerpStrategy", "Order on " + opportunity.pair.exchange1 + 
                          " failed to fill, attempting to close second position", true);
                
                exchange1_->cancelOrder(order1_id);
                
                // Close the second position
                Order closeOrder2;
                closeOrder2.symbol = opportunity.pair.symbol2;
                closeOrder2.side = side2 == OrderSide::BUY ? OrderSide::SELL : OrderSide::BUY;
                closeOrder2.type = OrderType::MARKET;
                closeOrder2.amount = adjusted_size;
                
                std::string close_id = exchange2_->placeOrder(closeOrder2);
                if (close_id.empty()) {
                    logMessage("CrossExchangePerpStrategy", "CRITICAL ERROR: Failed to close position on " + 
                              opportunity.pair.exchange2 + ", MANUAL INTERVENTION REQUIRED", true);
                }
                return false;
            }
        }
        
        // Record the trade in a persistent storage for position tracking
        logMessage("CrossExchangePerpStrategy", "Successfully executed trade on both exchanges");
        return true;
    } catch (const std::exception& e) {
        logMessage("CrossExchangePerpStrategy", 
                  "Error executing cross-exchange trade: " + std::string(e.what()), true);
        return false;
    }
}

// Helper method to check if we have sufficient margin for execution
bool CrossExchangePerpStrategy::checkMarginRequirements(const Order& order1, const Order& order2) {
    try {
        // Get account balances from both exchanges
        auto balance1 = retryApiCall("Get account balance from " + exchange1_->getName(),
            [this]() { return exchange1_->getAccountBalance(); });
            
        auto balance2 = retryApiCall("Get account balance from " + exchange2_->getName(),
            [this]() { return exchange2_->getAccountBalance(); });
        
        // Extract the relevant currencies based on the order symbols
        std::string base_currency1 = order1.symbol.substr(0, order1.symbol.find('/'));
        std::string quote_currency1 = order1.symbol.substr(order1.symbol.find('/') + 1);
        
        std::string base_currency2 = order2.symbol.substr(0, order2.symbol.find('/'));
        std::string quote_currency2 = order2.symbol.substr(order2.symbol.find('/') + 1);
        
        // Check available margin on exchange 1
        double required_margin1 = 0.0;
        if (order1.side == OrderSide::BUY) {
            // For longs, check quote currency (e.g., USDT)
            required_margin1 = order1.amount * order1.price * 1.1; // Add 10% buffer
            if (balance1.available.find(quote_currency1) == balance1.available.end() ||
                balance1.available[quote_currency1] < required_margin1) {
                logMessage("CrossExchangePerpStrategy", "Insufficient " + quote_currency1 + 
                          " balance on " + exchange1_->getName() + ": " + 
                          std::to_string(balance1.available[quote_currency1]) + 
                          ", required: " + std::to_string(required_margin1), true);
                return false;
            }
        } else {
            // For shorts, check base currency or margin balance
            if (balance1.available.find("USDT") == balance1.available.end() ||
                balance1.available["USDT"] < required_margin1) {
                logMessage("CrossExchangePerpStrategy", "Insufficient margin balance on " + 
                          exchange1_->getName(), true);
                return false;
            }
        }
        
        // Repeat for exchange 2
        double required_margin2 = 0.0;
        if (order2.side == OrderSide::BUY) {
            required_margin2 = order2.amount * order2.price * 1.1;
            if (balance2.available.find(quote_currency2) == balance2.available.end() ||
                balance2.available[quote_currency2] < required_margin2) {
                logMessage("CrossExchangePerpStrategy", "Insufficient " + quote_currency2 + 
                          " balance on " + exchange2_->getName(), true);
                return false;
            }
        } else {
            if (balance2.available.find("USDT") == balance2.available.end() ||
                balance2.available["USDT"] < required_margin2) {
                logMessage("CrossExchangePerpStrategy", "Insufficient margin balance on " + 
                          exchange2_->getName(), true);
                return false;
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        logMessage("CrossExchangePerpStrategy", 
                  "Error checking margin requirements: " + std::string(e.what()), true);
        return false;
    }
}

// Helper method to wait for an order to fill
bool CrossExchangePerpStrategy::waitForOrderFill(
    std::shared_ptr<ExchangeInterface> exchange, 
    const std::string& order_id, 
    int max_attempts) {
    
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            OrderStatus status = retryApiCall("Check order status",
                [&exchange, &order_id]() { return exchange->getOrderStatus(order_id); });
            
            if (status == OrderStatus::FILLED) {
                return true;
            } else if (status == OrderStatus::PARTIALLY_FILLED && attempt == max_attempts) {
                // Consider partially filled orders as success on the last attempt
                return true;
            } else if (status == OrderStatus::CANCELED || status == OrderStatus::REJECTED) {
                logMessage("CrossExchangePerpStrategy", "Order was rejected or cancelled", true);
                return false;
            }
            
            // If we reach here, order is still pending
            logMessage("CrossExchangePerpStrategy", "Order still pending (attempt " + 
                      std::to_string(attempt) + "/" + std::to_string(max_attempts) + 
                      "), waiting...");
        } catch (const std::exception& e) {
            logMessage("CrossExchangePerpStrategy", "Error checking order status: " + 
                      std::string(e.what()), true);
            // Continue trying despite error
        }
    }
    
    // If we get here, we've exhausted all attempts
    return false;
}

bool CrossExchangePerpStrategy::closePosition(const ArbitrageOpportunity& opportunity) {
    logMessage("CrossExchangePerpStrategy", "Attempting to close position for pair: " + 
              opportunity.pair.symbol1 + "/" + opportunity.pair.symbol2);
    
    try {
        // Get current positions on both exchanges with retry logic
        auto positions1 = retryApiCall("Get positions on " + exchange1_->getName(),
            [this]() { return exchange1_->getOpenPositions(); });
            
        auto positions2 = retryApiCall("Get positions on " + exchange2_->getName(),
            [this]() { return exchange2_->getOpenPositions(); });
        
        // Identify our positions related to this opportunity
        Position* pos1_ptr = nullptr;
        Position* pos2_ptr = nullptr;
        
        for (auto& pos : positions1) {
            if (pos.symbol == opportunity.pair.symbol1) {
                pos1_ptr = &pos;
                break;
            }
        }
        
        for (auto& pos : positions2) {
            if (pos.symbol == opportunity.pair.symbol2) {
                pos2_ptr = &pos;
                break;
            }
        }
        
        // If we couldn't find both positions, we might not have an open position for this opportunity
        if (!pos1_ptr || !pos2_ptr) {
            logMessage("CrossExchangePerpStrategy", "Could not find positions for this opportunity", true);
            
            // Get current account balances to check for any partially filled or failed positions
            auto balance1 = retryApiCall("Get account balance for " + exchange1_->getName(),
                [this]() { return exchange1_->getAccountBalance(); });
                
            auto balance2 = retryApiCall("Get account balance for " + exchange2_->getName(),
                [this]() { return exchange2_->getAccountBalance(); });
                
            // Extract base currencies from symbols
            std::string base_currency1 = opportunity.pair.symbol1.substr(0, opportunity.pair.symbol1.find('/'));
            std::string base_currency2 = opportunity.pair.symbol2.substr(0, opportunity.pair.symbol2.find('/'));
            
            // Check if we have base currency balance on either exchange
            bool has_balance1 = balance1.total.find(base_currency1) != balance1.total.end() && 
                              balance1.total[base_currency1] > 0.001;
            bool has_balance2 = balance2.total.find(base_currency2) != balance2.total.end() && 
                              balance2.total[base_currency2] > 0.001;
            
            // If we have balance on one exchange but not the other, we might have a failed partial execution
            if (has_balance1 && !pos1_ptr) {
                logMessage("CrossExchangePerpStrategy", "Found position balance on " + exchange1_->getName() + 
                          " but no position record. Attempting cleanup...");
                
                // Create a market order to close the position based on balance
                Order closeOrder;
                closeOrder.symbol = opportunity.pair.symbol1;
                closeOrder.side = OrderSide::SELL; // Assume long position if we have base currency
                closeOrder.type = OrderType::MARKET;
                closeOrder.amount = balance1.total[base_currency1];
                
                std::string close_id = retryApiCall("Place cleanup order on " + exchange1_->getName(),
                    [this, &closeOrder]() { return exchange1_->placeOrder(closeOrder); });
                    
                if (close_id.empty()) {
                    logMessage("CrossExchangePerpStrategy", "Failed to clean up position on " + 
                              exchange1_->getName(), true);
                } else {
                    logMessage("CrossExchangePerpStrategy", "Successfully cleaned up position on " + 
                              exchange1_->getName());
                }
            }
            
            if (has_balance2 && !pos2_ptr) {
                logMessage("CrossExchangePerpStrategy", "Found position balance on " + exchange2_->getName() + 
                          " but no position record. Attempting cleanup...");
                
                // Create a market order to close the position based on balance
                Order closeOrder;
                closeOrder.symbol = opportunity.pair.symbol2;
                closeOrder.side = OrderSide::SELL; // Assume long position if we have base currency
                closeOrder.type = OrderType::MARKET;
                closeOrder.amount = balance2.total[base_currency2];
                
                std::string close_id = retryApiCall("Place cleanup order on " + exchange2_->getName(),
                    [this, &closeOrder]() { return exchange2_->placeOrder(closeOrder); });
                    
                if (close_id.empty()) {
                    logMessage("CrossExchangePerpStrategy", "Failed to clean up position on " + 
                              exchange2_->getName(), true);
                } else {
                    logMessage("CrossExchangePerpStrategy", "Successfully cleaned up position on " + 
                              exchange2_->getName());
                }
            }
            
            // If we couldn't find positions and no cleanup was needed, signal that nothing was done
            if (!has_balance1 && !has_balance2) {
                logMessage("CrossExchangePerpStrategy", "No positions found to close");
                return false;
            }
            
            return true;
        }
        
        // Get current prices with retry logic
        double price1 = retryApiCall("Get closing price for " + opportunity.pair.symbol1,
            [this, &opportunity]() { return exchange1_->getPrice(opportunity.pair.symbol1); });
            
        double price2 = retryApiCall("Get closing price for " + opportunity.pair.symbol2,
            [this, &opportunity]() { return exchange2_->getPrice(opportunity.pair.symbol2); });
        
        // Calculate current spread
        double current_spread = std::abs(price1 - price2) / std::min(price1, price2) * 100.0;
        
        // Log position details and current market conditions
        std::stringstream ss;
        ss << "Position details - Exchange1: " << pos1_ptr->size << " @ " << pos1_ptr->entry_price
           << ", Exchange2: " << pos2_ptr->size << " @ " << pos2_ptr->entry_price
           << ", Current spread: " << current_spread << "%";
        logMessage("CrossExchangePerpStrategy", ss.str());
        
        // Calculate PnL before closing
        double unrealized_pnl1 = pos1_ptr->unrealized_pnl;
        double unrealized_pnl2 = pos2_ptr->unrealized_pnl;
        double total_pnl = unrealized_pnl1 + unrealized_pnl2;
        
        logMessage("CrossExchangePerpStrategy", "Unrealized PnL: " + std::to_string(total_pnl));
        
        // Determine closing sizes
        double close_size1 = std::abs(pos1_ptr->size);
        double close_size2 = std::abs(pos2_ptr->size);
        
        // Determine order sides for closing (opposite of position direction)
        OrderSide close_side1 = pos1_ptr->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        OrderSide close_side2 = pos2_ptr->size > 0 ? OrderSide::SELL : OrderSide::BUY;
        
        // Get order books to determine liquidity for closing
        OrderBook ob1 = retryApiCall("Get closing order book for " + opportunity.pair.symbol1,
            [this, &opportunity]() { return exchange1_->getOrderBook(opportunity.pair.symbol1, 5); });
            
        OrderBook ob2 = retryApiCall("Get closing order book for " + opportunity.pair.symbol2,
            [this, &opportunity]() { return exchange2_->getOrderBook(opportunity.pair.symbol2, 5); });
        
        // Calculate total liquidity available for our closing side
        double liq1 = 0.0, liq2 = 0.0;
        
        if (close_side1 == OrderSide::SELL) {
            for (const auto& level : ob1.bids) {
                liq1 += level.amount;
                if (liq1 >= close_size1 * 1.5) break; // 50% buffer
            }
        } else {
            for (const auto& level : ob1.asks) {
                liq1 += level.amount;
                if (liq1 >= close_size1 * 1.5) break; // 50% buffer
            }
        }
        
        if (close_side2 == OrderSide::SELL) {
            for (const auto& level : ob2.bids) {
                liq2 += level.amount;
                if (liq2 >= close_size2 * 1.5) break; // 50% buffer
            }
        } else {
            for (const auto& level : ob2.asks) {
                liq2 += level.amount;
                if (liq2 >= close_size2 * 1.5) break; // 50% buffer
            }
        }
        
        // Check if there's sufficient liquidity for closing
        if (liq1 < close_size1) {
            logMessage("CrossExchangePerpStrategy", "Warning: Insufficient liquidity for closing on " + 
                      exchange1_->getName() + ". Available: " + std::to_string(liq1) + 
                      ", Required: " + std::to_string(close_size1));
            close_size1 = liq1 * 0.75; // Use 75% of available liquidity
        }
        
        if (liq2 < close_size2) {
            logMessage("CrossExchangePerpStrategy", "Warning: Insufficient liquidity for closing on " + 
                      exchange2_->getName() + ". Available: " + std::to_string(liq2) + 
                      ", Required: " + std::to_string(close_size2));
            close_size2 = liq2 * 0.75; // Use 75% of available liquidity
        }
        
        // Create order objects for closing
        Order closeOrder1;
        closeOrder1.symbol = opportunity.pair.symbol1;
        closeOrder1.side = close_side1;
        closeOrder1.type = OrderType::MARKET;
        closeOrder1.amount = close_size1;
        
        Order closeOrder2;
        closeOrder2.symbol = opportunity.pair.symbol2;
        closeOrder2.side = close_side2;
        closeOrder2.type = OrderType::MARKET;
        closeOrder2.amount = close_size2;
        
        // Determine which position to close first based on market conditions
        bool close_exchange1_first = false;
        
        // If liquidity is significantly different, close less liquid exchange first
        if (liq1 / close_size1 < liq2 / close_size2 * 0.7) {
            close_exchange1_first = true;
        } else if (liq2 / close_size2 < liq1 / close_size1 * 0.7) {
            close_exchange1_first = false;
        }
        // Otherwise, close the larger position or more volatile market first
        else if (std::abs(pos1_ptr->size * price1) > std::abs(pos2_ptr->size * price2)) {
            close_exchange1_first = true;
        }
        
        std::string close1_id, close2_id;
        
        // Execute closing orders with a sequential approach
        if (close_exchange1_first) {
            logMessage("CrossExchangePerpStrategy", "Closing position on " + 
                      exchange1_->getName() + " first");
            
            // Place order on exchange 1
            close1_id = retryApiCall("Place closing order on " + exchange1_->getName(),
                [this, &closeOrder1]() { return exchange1_->placeOrder(closeOrder1); });
            
            if (close1_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to close position on " + 
                          exchange1_->getName(), true);
                return false;
            }
            
            // Wait for the order to fill
            bool close1_filled = waitForOrderFill(exchange1_, close1_id, 3);
            if (!close1_filled) {
                logMessage("CrossExchangePerpStrategy", "Closing order on " + 
                          exchange1_->getName() + " did not fill completely", true);
                // Continue to exchange 2 even if first order didn't fill completely
            }
            
            // Place order on exchange 2
            close2_id = retryApiCall("Place closing order on " + exchange2_->getName(),
                [this, &closeOrder2]() { return exchange2_->placeOrder(closeOrder2); });
            
            if (close2_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to close position on " + 
                          exchange2_->getName(), true);
                return false;
            }
        } else {
            logMessage("CrossExchangePerpStrategy", "Closing position on " + 
                      exchange2_->getName() + " first");
            
            // Place order on exchange 2
            close2_id = retryApiCall("Place closing order on " + exchange2_->getName(),
                [this, &closeOrder2]() { return exchange2_->placeOrder(closeOrder2); });
            
            if (close2_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to close position on " + 
                          exchange2_->getName(), true);
                return false;
            }
            
            // Wait for the order to fill
            bool close2_filled = waitForOrderFill(exchange2_, close2_id, 3);
            if (!close2_filled) {
                logMessage("CrossExchangePerpStrategy", "Closing order on " + 
                          exchange2_->getName() + " did not fill completely", true);
                // Continue to exchange 1 even if second order didn't fill completely
            }
            
            // Place order on exchange 1
            close1_id = retryApiCall("Place closing order on " + exchange1_->getName(),
                [this, &closeOrder1]() { return exchange1_->placeOrder(closeOrder1); });
            
            if (close1_id.empty()) {
                logMessage("CrossExchangePerpStrategy", "Failed to close position on " + 
                          exchange1_->getName(), true);
                return false;
            }
        }
        
        // Verify final position state
        bool success = verifyPositionClosed(opportunity);
        
        if (success) {
            logMessage("CrossExchangePerpStrategy", "Successfully closed arbitrage position between " + 
                      exchange1_->getName() + " and " + exchange2_->getName() + 
                      " for " + opportunity.pair.symbol1 + " / " + opportunity.pair.symbol2);
        } else {
            logMessage("CrossExchangePerpStrategy", "Position may not be fully closed, please verify", true);
        }
        
        return success;
    } catch (const std::exception& e) {
        logMessage("CrossExchangePerpStrategy", 
                  "Error closing cross-exchange position: " + std::string(e.what()), true);
        return false;
    }
}

// Helper method to verify that positions are fully closed
bool CrossExchangePerpStrategy::verifyPositionClosed(const ArbitrageOpportunity& opportunity) {
    try {
        // Get current positions after closing attempt
        auto positions1 = retryApiCall("Get positions after closing on " + exchange1_->getName(),
            [this]() { return exchange1_->getOpenPositions(); });
            
        auto positions2 = retryApiCall("Get positions after closing on " + exchange2_->getName(),
            [this]() { return exchange2_->getOpenPositions(); });
        
        // Check if positions still exist
        bool pos1_exists = false;
        bool pos2_exists = false;
        
        for (const auto& pos : positions1) {
            if (pos.symbol == opportunity.pair.symbol1 && std::abs(pos.size) > 0.001) {
                pos1_exists = true;
                logMessage("CrossExchangePerpStrategy", "Position still exists on " + 
                          exchange1_->getName() + ": " + std::to_string(pos.size));
                break;
            }
        }
        
        for (const auto& pos : positions2) {
            if (pos.symbol == opportunity.pair.symbol2 && std::abs(pos.size) > 0.001) {
                pos2_exists = true;
                logMessage("CrossExchangePerpStrategy", "Position still exists on " + 
                          exchange2_->getName() + ": " + std::to_string(pos.size));
                break;
            }
        }
        
        return !pos1_exists && !pos2_exists;
    } catch (const std::exception& e) {
        logMessage("CrossExchangePerpStrategy", 
                  "Error verifying position close: " + std::string(e.what()), true);
        return false;
    }
}

void CrossExchangePerpStrategy::monitorPositions() {
    try {
        logMessage("CrossExchangePerpStrategy", "Monitoring active positions");
        
        // Get all current positions with retry logic
        auto positions1 = retryApiCall("Get positions from " + exchange1_->getName(),
            [this]() { return exchange1_->getOpenPositions(); });
            
        auto positions2 = retryApiCall("Get positions from " + exchange2_->getName(),
            [this]() { return exchange2_->getOpenPositions(); });
        
        if (positions1.empty() && positions2.empty()) {
            logMessage("CrossExchangePerpStrategy", "No active positions to monitor");
            return;
        }
        
        // Group positions that are part of the same arbitrage trade
        std::map<std::string, std::pair<Position*, Position*>> paired_positions;
        std::map<std::string, double> position_entry_spreads; // For tracking historical spreads
        
        // First, add all positions from exchange1
        for (auto& pos1 : positions1) {
            std::string key = pos1.symbol; // Use symbol as key for now
            paired_positions[key] = {&pos1, nullptr};
            
            // Log position details
            std::stringstream ss;
            ss << "Found position on " << exchange1_->getName() << ": " 
               << pos1.symbol << ", Size: " << pos1.size
               << ", Entry price: " << pos1.entry_price
               << ", PnL: " << pos1.unrealized_pnl;
            logMessage("CrossExchangePerpStrategy", ss.str());
        }
        
        // Then try to pair with positions from exchange2
        for (auto& pos2 : positions2) {
            // Look for direct symbol match or matching base currency
            std::string base_currency2 = pos2.symbol.substr(0, pos2.symbol.find('/'));
            bool found_pair = false;
            
            for (auto& [key, pair] : paired_positions) {
                std::string base_currency1 = key.substr(0, key.find('/'));
                
                // If this position hasn't been paired and currencies match
                if (!pair.second && (key == pos2.symbol || base_currency1 == base_currency2)) {
                    pair.second = &pos2;
                    found_pair = true;
                    
                    std::stringstream ss;
                    ss << "Paired position on " << exchange2_->getName() << ": " 
                       << pos2.symbol << ", Size: " << pos2.size
                       << ", Entry price: " << pos2.entry_price
                       << ", PnL: " << pos2.unrealized_pnl;
                    logMessage("CrossExchangePerpStrategy", ss.str());
                    break;
                }
            }
            
            // If no pair found, create a new entry
            if (!found_pair) {
                std::string key = pos2.symbol;
                paired_positions[key] = {nullptr, &pos2};
                
                std::stringstream ss;
                ss << "Found position on " << exchange2_->getName() << ": " 
                   << pos2.symbol << ", Size: " << pos2.size
                   << ", Entry price: " << pos2.entry_price
                   << ", PnL: " << pos2.unrealized_pnl;
                logMessage("CrossExchangePerpStrategy", ss.str());
            }
        }
        
        // Check each pair for risk metrics
        for (auto& [symbol, pos_pair] : paired_positions) {
            Position* pos1 = pos_pair.first;
            Position* pos2 = pos_pair.second;
            
            // Skip unpaired positions
            if (!pos1 || !pos2) {
                if (pos1) {
                    logMessage("CrossExchangePerpStrategy", "Warning: Unpaired position on " + 
                              exchange1_->getName() + ": " + pos1->symbol, true);
                } else if (pos2) {
                    logMessage("CrossExchangePerpStrategy", "Warning: Unpaired position on " + 
                              exchange2_->getName() + ": " + pos2->symbol, true);
                }
                continue;
            }
            
            // Get current prices
            std::string symbol1 = pos1->symbol;
            std::string symbol2 = pos2->symbol;
            
            double price1 = retryApiCall("Get current price for " + symbol1,
                [this, &symbol1]() { return exchange1_->getPrice(symbol1); });
                
            double price2 = retryApiCall("Get current price for " + symbol2,
                [this, &symbol2]() { return exchange2_->getPrice(symbol2); });
            
            // Calculate current spread
            double current_spread = std::abs(price1 - price2) / std::min(price1, price2) * 100.0;
            
            // Calculate entry spread (if not stored)
            double entry_spread = 0.0;
            if (position_entry_spreads.find(symbol) == position_entry_spreads.end()) {
                entry_spread = std::abs(pos1->entry_price - pos2->entry_price) / 
                             std::min(pos1->entry_price, pos2->entry_price) * 100.0;
                position_entry_spreads[symbol] = entry_spread;
            } else {
                entry_spread = position_entry_spreads[symbol];
            }
            
            // Check if spread has widened too much (indicating increased risk)
            const double MAX_SPREAD_INCREASE = 0.5; // 0.5% increase threshold
            if (current_spread > entry_spread + MAX_SPREAD_INCREASE) {
                logMessage("CrossExchangePerpStrategy", "Warning: Spread has widened from " + 
                          std::to_string(entry_spread) + "% to " + std::to_string(current_spread) + 
                          "% for " + symbol1 + " / " + symbol2, true);
                
                // Calculate current spread PnL impact
                double spread_pnl = (entry_spread - current_spread) * std::abs(pos1->size) * price1 / 100.0;
                
                // Check funding rates to see if continued holding makes sense
                FundingRate funding1 = retryApiCall("Get funding rate for " + symbol1,
                    [this, &symbol1]() { return exchange1_->getFundingRate(symbol1); });
                    
                FundingRate funding2 = retryApiCall("Get funding rate for " + symbol2,
                    [this, &symbol2]() { return exchange2_->getFundingRate(symbol2); });
                
                // Calculate expected funding payments
                double hours_per_year = 24.0 * 365.0;
                double payments_per_year1 = hours_per_year / funding1.payment_interval.count();
                double payments_per_year2 = hours_per_year / funding2.payment_interval.count();
                
                double daily_funding1 = funding1.rate * (24.0 / funding1.payment_interval.count());
                double daily_funding2 = funding2.rate * (24.0 / funding2.payment_interval.count());
                double net_daily_funding = (daily_funding1 - daily_funding2) * 100.0; // As percentage
                
                // Calculate days to recover spread loss
                double daily_funding_profit = net_daily_funding * std::abs(pos1->size) * price1 / 100.0;
                double days_to_recover = (daily_funding_profit > 0.0001) ? 
                                      std::abs(spread_pnl) / daily_funding_profit : 999.0;
                
                // If recovery time is too long or funding advantage is gone, reduce position
                if (days_to_recover > 7.0 || daily_funding_profit <= 0.0001) {
                    logMessage("CrossExchangePerpStrategy", "Position no longer viable - " + 
                              std::to_string(days_to_recover) + " days to recover spread loss. " +
                              "Daily funding profit: " + std::to_string(daily_funding_profit) +
                              ". Reducing position.");
                    
                    // Calculate reduction size (e.g., 25% of position)
                    double perp_reduce_size = std::abs(pos1->size) * 0.25;
                    double spot_reduce_size = std::abs(pos2->size) * 0.25;
                    
                    // Determine order sides for reduction
                    OrderSide close_side1 = pos1->size > 0 ? OrderSide::SELL : OrderSide::BUY;
                    OrderSide close_side2 = pos2->size > 0 ? OrderSide::SELL : OrderSide::BUY;
                    
                    try {
                        // Create and place reduction orders
                        Order reduceOrder1;
                        reduceOrder1.symbol = symbol1;
                        reduceOrder1.side = close_side1;
                        reduceOrder1.type = OrderType::MARKET;
                        reduceOrder1.amount = perp_reduce_size;
                        
                        Order reduceOrder2;
                        reduceOrder2.symbol = symbol2;
                        reduceOrder2.side = close_side2;
                        reduceOrder2.type = OrderType::MARKET;
                        reduceOrder2.amount = spot_reduce_size;
                        
                        // Execute on exchange with better liquidity first
                        bool exchange1_first = (pos1->size > pos2->size);
                        
                        if (exchange1_first) {
                            // Close on exchange 1 first
                            std::string order1_id = retryApiCall("Place reduction order on " + exchange1_->getName(),
                                [this, &reduceOrder1]() { return exchange1_->placeOrder(reduceOrder1); });
                                
                            if (order1_id.empty()) {
                                logMessage("CrossExchangePerpStrategy", "Failed to reduce position on " + 
                                          exchange1_->getName(), true);
                                return;
                            }
                            
                            // Wait for fill
                            bool order1_filled = waitForOrderFill(exchange1_, order1_id, 3);
                            if (!order1_filled) {
                                logMessage("CrossExchangePerpStrategy", "Reduction order on " + 
                                          exchange1_->getName() + " did not fill completely", true);
                                return;
                            }
                            
                            // Now reduce on exchange 2
                            std::string order2_id = retryApiCall("Place reduction order on " + exchange2_->getName(),
                                [this, &reduceOrder2]() { return exchange2_->placeOrder(reduceOrder2); });
                                
                            if (order2_id.empty()) {
                                logMessage("CrossExchangePerpStrategy", "Failed to reduce position on " + 
                                          exchange2_->getName() + ", WARNING: HEDGE IMBALANCE", true);
                                return;
                            }
                        } else {
                            // Close on exchange 2 first
                            std::string order2_id = retryApiCall("Place reduction order on " + exchange2_->getName(),
                                [this, &reduceOrder2]() { return exchange2_->placeOrder(reduceOrder2); });
                                
                            if (order2_id.empty()) {
                                logMessage("CrossExchangePerpStrategy", "Failed to reduce position on " + 
                                          exchange2_->getName() + ", WARNING: HEDGE IMBALANCE", true);
                                return;
                            }
                            
                            // Wait for fill
                            bool order2_filled = waitForOrderFill(exchange2_, order2_id, 3);
                            if (!order2_filled) {
                                logMessage("CrossExchangePerpStrategy", "Reduction order on " + 
                                          exchange2_->getName() + " did not fill completely", true);
                                return;
                            }
                            
                            // Now reduce on exchange 1
                            std::string order1_id = retryApiCall("Place reduction order on " + exchange1_->getName(),
                                [this, &reduceOrder1]() { return exchange1_->placeOrder(reduceOrder1); });
                                
                            if (order1_id.empty()) {
                                logMessage("CrossExchangePerpStrategy", "Failed to reduce position on " + 
                                          exchange1_->getName() + ", WARNING: HEDGE IMBALANCE", true);
                                return;
                            }
                        }
                        
                        logMessage("CrossExchangePerpStrategy", "Successfully reduced position by 25%");
                    } catch (const std::exception& e) {
                        logMessage("CrossExchangePerpStrategy", 
                                  "Error reducing position: " + std::string(e.what()), true);
                    }
                }
            }
            
            // Check funding rate status
            FundingRate funding1 = retryApiCall("Get funding rate for " + symbol1,
                [this, &symbol1]() { return exchange1_->getFundingRate(symbol1); });
                
            FundingRate funding2 = retryApiCall("Get funding rate for " + symbol2,
                [this, &symbol2]() { return exchange2_->getFundingRate(symbol2); });
            
            // Check time until next funding payment
            auto now = std::chrono::system_clock::now();
            int time_to_funding1 = static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(
                funding1.next_payment - now).count());
            int time_to_funding2 = static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(
                funding2.next_payment - now).count());
            
            // Log upcoming funding payments
            if (time_to_funding1 < 60 || time_to_funding2 < 60) { // Less than an hour
                std::stringstream ss;
                ss << "Upcoming funding payments - " << symbol1 << ": " 
                   << (funding1.rate * 100.0) << "% in " << (time_to_funding1 / 60.0) << " hours, "
                   << symbol2 << ": " << (funding2.rate * 100.0) << "% in " 
                   << (time_to_funding2 / 60.0) << " hours";
                logMessage("CrossExchangePerpStrategy", ss.str());
                
                // Calculate expected funding payments
                double funding_payment1 = funding1.rate * std::abs(pos1->size) * price1;
                double funding_payment2 = funding2.rate * std::abs(pos2->size) * price2;
                double total_payment = funding_payment1 - funding_payment2;
                
                ss.str("");
                ss << "Expected funding payment: " << total_payment << " USD";
                logMessage("CrossExchangePerpStrategy", ss.str());
            }
            
            // If funding rates have reversed and the reversal seems permanent (not just a temporary fluctuation)
            // and there's an upcoming payment, consider closing the position
            if ((funding1.rate > 0 && funding2.rate < 0 && pos1->size < 0) || 
                (funding1.rate < 0 && funding2.rate > 0 && pos1->size > 0)) {
                
                double rate_diff = std::abs(funding1.rate - funding2.rate);
                double predicted_diff = std::abs(funding1.predicted_rate - funding2.predicted_rate);
                
                // Only close if the reversal is significant and persists in predictions
                if (rate_diff > 0.0005 && predicted_diff > 0.0003) {
                    std::stringstream message;
                    message << "Funding rates have reversed direction. "
                           << "Current diff: " << rate_diff
                           << ", Predicted diff: " << predicted_diff
                           << ". Closing position.";
                    logMessage("CrossExchangePerpStrategy", message.str());
                    
                    // Create an ArbitrageOpportunity to pass to closePosition
                    ArbitrageOpportunity opp;
                    opp.pair.exchange1 = exchange1_->getName();
                    opp.pair.symbol1 = symbol1;
                    opp.pair.market_type1 = MarketType::PERPETUAL;
                    opp.pair.exchange2 = exchange2_->getName();
                    opp.pair.symbol2 = symbol2;
                    opp.pair.market_type2 = MarketType::PERPETUAL;
                    
                    // Close the position
                    bool closed = closePosition(opp);
                    
                    if (closed) {
                        logMessage("CrossExchangePerpStrategy", "Successfully closed position after funding rate reversal");
                    } else {
                        logMessage("CrossExchangePerpStrategy", "Failed to close position after funding rate reversal", true);
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        logMessage("CrossExchangePerpStrategy", 
                  "Error monitoring cross-exchange positions: " + std::string(e.what()), true);
    }
}

} // namespace funding
