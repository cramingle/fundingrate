#include <strategy/strategy_factory.h>
#include <strategy/arbitrage_strategy.h>
#include <strategy/composite_strategy.h>
#include <config/config_manager.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <chrono>
#include <mutex>
#include <memory>
#include <functional>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <thread>

namespace funding {

// Global mutex for thread-safe logging
static std::mutex g_strategy_factory_mutex;

// Helper function for thread-safe logging
void logStrategyFactory(const std::string& message, bool is_error = false) {
    std::lock_guard<std::mutex> lock(g_strategy_factory_mutex);
    
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    // Format time using stringstream
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    ss << (is_error ? " [ERROR] " : " [INFO] ") << "StrategyFactory: " << message;
    
    // Output to console
    if (is_error) {
        std::cerr << ss.str() << std::endl;
    } else {
        std::cout << ss.str() << std::endl;
    }
    
    // In a production system, we would also log to a file or logging service
}

// Helper function to retry operations that might fail temporarily
template<typename Func>
auto retryOperation(const std::string& operation_name, Func&& func, int max_retries = 3) 
    -> decltype(func()) {
    
    int retries = 0;
    std::chrono::milliseconds delay(100); // Start with 100ms delay
    
    while (true) {
        try {
            return func();
        } catch (const std::exception& e) {
            if (++retries > max_retries) {
                logStrategyFactory("Operation '" + operation_name + "' failed after " + 
                                  std::to_string(max_retries) + " retries: " + e.what(), true);
                throw; // Re-throw after max retries
            }
            
            logStrategyFactory("Retry " + std::to_string(retries) + "/" + 
                              std::to_string(max_retries) + " for operation '" + 
                              operation_name + "': " + e.what(), true);
            
            // Exponential backoff
            std::this_thread::sleep_for(delay);
            delay *= 2; // Double the delay for next retry
        }
    }
}

// Create a composite strategy for all same-exchange spot-perp opportunities
std::unique_ptr<ArbitrageStrategy> createSameExchangeSpotPerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate,
    double min_expected_profit) {
    
    if (exchanges.empty()) {
        logStrategyFactory("No exchanges provided for SameExchangeSpotPerpStrategy", true);
        return nullptr;
    }
    
    logStrategyFactory("Creating SameExchangeSpotPerpStrategy with " + 
                      std::to_string(exchanges.size()) + " exchanges, " +
                      "min_funding_rate=" + std::to_string(min_funding_rate) + ", " +
                      "min_expected_profit=" + std::to_string(min_expected_profit));
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (const auto& exchange : exchanges) {
        try {
            auto strategy = std::make_unique<SameExchangeSpotPerpStrategy>(exchange);
            
            // Set the minimum funding rate and expected profit
            strategy->setMinFundingRate(min_funding_rate);
            strategy->setMinExpectedProfit(min_expected_profit);
            
            logStrategyFactory("StrategyFactory: Successfully created SameExchangeSpotPerpStrategy for " + 
                              exchange->getName() + " with min_funding_rate=" + 
                              std::to_string(min_funding_rate) + ", min_expected_profit=" + 
                              std::to_string(min_expected_profit));
            
            strategies.push_back(std::move(strategy));
        } catch (const std::exception& e) {
            logStrategyFactory("StrategyFactory: Failed to create SameExchangeSpotPerpStrategy for " + 
                              exchange->getName() + ": " + e.what(), true);
        }
    }
    
    if (strategies.empty()) {
        logStrategyFactory("StrategyFactory: No valid SameExchangeSpotPerpStrategy instances created", true);
        return nullptr;
    }
    
    if (strategies.size() == 1) {
        logStrategyFactory("Returning single SameExchangeSpotPerpStrategy for " + 
                          exchanges[0]->getName());
        return std::move(strategies[0]);
    }
    
    logStrategyFactory("StrategyFactory: Creating composite strategy with " + 
                      std::to_string(strategies.size()) + " SameExchangeSpotPerpStrategy instances");
    
    return std::make_unique<CompositeStrategy>(std::move(strategies));
}

// Create a composite strategy for all cross-exchange perp-perp opportunities
std::unique_ptr<ArbitrageStrategy> createCrossExchangePerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate,
    double min_expected_profit) {
    
    if (exchanges.size() < 2) {
        logStrategyFactory("Need at least 2 exchanges for CrossExchangePerpStrategy", true);
        return nullptr;
    }
    
    logStrategyFactory("Creating CrossExchangePerpStrategy with " + 
                      std::to_string(exchanges.size()) + " exchanges, " +
                      "min_funding_rate=" + std::to_string(min_funding_rate) + ", " +
                      "min_expected_profit=" + std::to_string(min_expected_profit));
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (size_t i = 0; i < exchanges.size(); ++i) {
        for (size_t j = i + 1; j < exchanges.size(); ++j) {
            try {
                auto strategy = std::make_unique<CrossExchangePerpStrategy>(exchanges[i], exchanges[j]);
                
                // Set the minimum funding rate and expected profit
                strategy->setMinFundingRate(min_funding_rate);
                strategy->setMinExpectedProfit(min_expected_profit);
                
                logStrategyFactory("StrategyFactory: Successfully created CrossExchangePerpStrategy for " + 
                                  exchanges[i]->getName() + " and " + exchanges[j]->getName() + 
                                  " with min_funding_rate=" + std::to_string(min_funding_rate) + 
                                  ", min_expected_profit=" + std::to_string(min_expected_profit));
                
                strategies.push_back(std::move(strategy));
            } catch (const std::exception& e) {
                logStrategyFactory("StrategyFactory: Failed to create CrossExchangePerpStrategy for " + 
                                  exchanges[i]->getName() + " and " + exchanges[j]->getName() + ": " + e.what(), true);
            }
        }
    }
    
    if (strategies.empty()) {
        logStrategyFactory("StrategyFactory: No valid CrossExchangePerpStrategy instances created", true);
        return nullptr;
    }
    
    if (strategies.size() == 1) {
        logStrategyFactory("Returning single CrossExchangePerpStrategy");
        return std::move(strategies[0]);
    }
    
    logStrategyFactory("StrategyFactory: Creating composite strategy with " + 
                      std::to_string(strategies.size()) + " CrossExchangePerpStrategy instances");
    
    return std::make_unique<CompositeStrategy>(std::move(strategies));
}

// Create a composite strategy for all cross-exchange spot-perp opportunities
std::unique_ptr<ArbitrageStrategy> createCrossExchangeSpotPerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate,
    double min_expected_profit) {
    
    if (exchanges.size() < 2) {
        logStrategyFactory("Need at least 2 exchanges for CrossExchangeSpotPerpStrategy", true);
        return nullptr;
    }
    
    logStrategyFactory("Creating CrossExchangeSpotPerpStrategy with " + 
                      std::to_string(exchanges.size()) + " exchanges, " +
                      "min_funding_rate=" + std::to_string(min_funding_rate) + ", " +
                      "min_expected_profit=" + std::to_string(min_expected_profit));
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (size_t i = 0; i < exchanges.size(); ++i) {
        for (size_t j = 0; j < exchanges.size(); ++j) {
            if (i == j) continue;
            
            try {
                auto strategy = std::make_unique<CrossExchangeSpotPerpStrategy>(
                    exchanges[i], exchanges[j]);
                
                // Set the minimum funding rate and expected profit
                strategy->setMinFundingRate(min_funding_rate);
                strategy->setMinExpectedProfit(min_expected_profit);
                
                logStrategyFactory("StrategyFactory: Successfully created CrossExchangeSpotPerpStrategy for " + 
                                  exchanges[i]->getName() + " (spot) and " + exchanges[j]->getName() + " (perp) with min_funding_rate=" + 
                                  std::to_string(min_funding_rate) + ", min_expected_profit=" + 
                                  std::to_string(min_expected_profit));
                
                strategies.push_back(std::move(strategy));
            } catch (const std::exception& e) {
                logStrategyFactory("StrategyFactory: Failed to create CrossExchangeSpotPerpStrategy for " + 
                                  exchanges[i]->getName() + " (spot) and " + exchanges[j]->getName() + " (perp): " + e.what(), true);
            }
        }
    }
    
    if (strategies.empty()) {
        logStrategyFactory("StrategyFactory: No valid CrossExchangeSpotPerpStrategy instances created", true);
        return nullptr;
    }
    
    if (strategies.size() == 1) {
        logStrategyFactory("Returning single CrossExchangeSpotPerpStrategy");
        return std::move(strategies[0]);
    }
    
    logStrategyFactory("StrategyFactory: Creating composite strategy with " + 
                      std::to_string(strategies.size()) + " CrossExchangeSpotPerpStrategy instances");
    
    return std::make_unique<CompositeStrategy>(std::move(strategies));
}

// Creates a specific strategy instance based on provided configuration
std::unique_ptr<ArbitrageStrategy> createStrategy(
    const std::string& strategy_type,
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate,
    double min_expected_profit) {
    
    logStrategyFactory("StrategyFactory: Creating strategy of type '" + strategy_type + 
                      "' with min_funding_rate=" + std::to_string(min_funding_rate) + 
                      ", min_expected_profit=" + std::to_string(min_expected_profit));
    
    if (strategy_type == "same_exchange_spot_perp") {
        return createSameExchangeSpotPerpStrategy(exchanges, min_funding_rate, min_expected_profit);
    } else if (strategy_type == "cross_exchange_perp") {
        return createCrossExchangePerpStrategy(exchanges, min_funding_rate, min_expected_profit);
    } else if (strategy_type == "cross_exchange_spot_perp") {
        return createCrossExchangeSpotPerpStrategy(exchanges, min_funding_rate, min_expected_profit);
    } else {
        logStrategyFactory("StrategyFactory: Unknown strategy type: " + strategy_type, true);
        return nullptr;
    }
}

// Helper function to create all strategies from the configuration
std::vector<std::unique_ptr<ArbitrageStrategy>> createAllStrategies(
    const BotConfig& config,
    const std::map<std::string, std::shared_ptr<ExchangeInterface>>& exchange_map) {
    
    logStrategyFactory("StrategyFactory: Creating all strategies from configuration with " + 
                      std::to_string(exchange_map.size()) + " exchanges and " +
                      std::to_string(config.strategies.size()) + " strategy configs");
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    std::vector<std::shared_ptr<ExchangeInterface>> exchanges;
    
    // Convert exchange map to vector
    for (const auto& [name, exchange] : exchange_map) {
        exchanges.push_back(exchange);
    }
    
    // Create strategies based on configuration
    for (const auto& strategy_config : config.strategies) {
        double min_funding_rate = strategy_config.min_funding_rate;
        double min_expected_profit = strategy_config.min_expected_profit;
        
        // Convert enum to string
        std::string strategy_type;
        switch (strategy_config.type) {
            case StrategyType::SAME_EXCHANGE_SPOT_PERP:
                strategy_type = "same_exchange_spot_perp";
                break;
            case StrategyType::CROSS_EXCHANGE_PERP_PERP:
                strategy_type = "cross_exchange_perp";
                break;
            case StrategyType::CROSS_EXCHANGE_SPOT_PERP:
                strategy_type = "cross_exchange_spot_perp";
                break;
            default:
                logStrategyFactory("StrategyFactory: Unknown strategy type enum value: " + 
                                  std::to_string(static_cast<int>(strategy_config.type)), true);
                continue;
        }
        
        auto strategy = createStrategy(
            strategy_type, 
            exchanges, 
            min_funding_rate,
            min_expected_profit);
        
        if (strategy) {
            logStrategyFactory("StrategyFactory: Added " + strategy_type + 
                              " strategy to the bot with min_funding_rate=" + std::to_string(min_funding_rate) + 
                              ", min_expected_profit=" + std::to_string(min_expected_profit));
            
            strategies.push_back(std::move(strategy));
        } else {
            logStrategyFactory("StrategyFactory: Failed to create " + strategy_type + " strategy", true);
        }
    }
    
    logStrategyFactory("StrategyFactory: Created " + std::to_string(strategies.size()) + " strategies in total");
    
    return strategies;
}

} // namespace funding 