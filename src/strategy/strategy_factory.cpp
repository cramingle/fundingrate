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
    
    // Create individual strategies for each exchange
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (const auto& exchange : exchanges) {
        // Skip exchanges that don't support both spot and perpetual markets
        try {
            auto spot_instruments = exchange->getAvailableInstruments(MarketType::SPOT);
            auto perp_instruments = exchange->getAvailableInstruments(MarketType::PERPETUAL);
            
            if (spot_instruments.empty() || perp_instruments.empty()) {
                logStrategyFactory("Skipping exchange " + exchange->getName() + 
                                  " for SameExchangeSpotPerpStrategy: missing required market types");
                continue;
            }
        } catch (const std::exception& e) {
            logStrategyFactory("Error checking market types for " + exchange->getName() + 
                              ": " + e.what(), true);
            continue;
        }
        
        // Create a strategy for this exchange with retry mechanism
        try {
            auto create_strategy = [&]() {
                auto strategy = std::make_unique<SameExchangeSpotPerpStrategy>(exchange);
                
                // Configure strategy parameters
                strategy->setMinFundingRate(min_funding_rate);
                strategy->setMinExpectedProfit(min_expected_profit);
                
                return strategy;
            };
            
            auto strategy = retryOperation("Create SameExchangeSpotPerpStrategy for " + 
                                         exchange->getName(), create_strategy);
            
            strategies.push_back(std::move(strategy));
            logStrategyFactory("Successfully created SameExchangeSpotPerpStrategy for " + 
                              exchange->getName() + " with min_funding_rate=" + 
                              std::to_string(min_funding_rate) + ", min_expected_profit=" + 
                              std::to_string(min_expected_profit));
        } catch (const std::exception& e) {
            logStrategyFactory("Failed to create SameExchangeSpotPerpStrategy for " + 
                              exchange->getName() + ": " + e.what(), true);
        }
    }
    
    if (strategies.empty()) {
        logStrategyFactory("No valid SameExchangeSpotPerpStrategy instances created", true);
        return nullptr;
    }
    
    // If only one strategy, return it directly
    if (strategies.size() == 1) {
        logStrategyFactory("Returning single SameExchangeSpotPerpStrategy for " + 
                          exchanges[0]->getName());
        return std::move(strategies[0]);
    }
    
    // Otherwise, create a composite strategy
    logStrategyFactory("Creating composite strategy with " + 
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
    
    // Create strategies for all pairs of exchanges
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    // Track exchanges that support perpetual markets
    std::vector<std::shared_ptr<ExchangeInterface>> valid_exchanges;
    for (const auto& exchange : exchanges) {
        try {
            auto perp_instruments = exchange->getAvailableInstruments(MarketType::PERPETUAL);
            if (!perp_instruments.empty()) {
                valid_exchanges.push_back(exchange);
            } else {
                logStrategyFactory("Skipping exchange " + exchange->getName() + 
                                  " for CrossExchangePerpStrategy: no perpetual instruments");
            }
        } catch (const std::exception& e) {
            logStrategyFactory("Error checking perpetual instruments for " + 
                              exchange->getName() + ": " + e.what(), true);
        }
    }
    
    if (valid_exchanges.size() < 2) {
        logStrategyFactory("Need at least 2 exchanges with perpetual markets for CrossExchangePerpStrategy", true);
        return nullptr;
    }
    
    // Create strategies for all pairs of valid exchanges
    for (size_t i = 0; i < valid_exchanges.size(); ++i) {
        for (size_t j = i + 1; j < valid_exchanges.size(); ++j) {
            try {
                auto create_strategy = [&]() {
                    auto strategy = std::make_unique<CrossExchangePerpStrategy>(
                        valid_exchanges[i], valid_exchanges[j]);
                    
                    // Configure strategy parameters
                    strategy->setMinFundingRate(min_funding_rate);
                    strategy->setMinExpectedProfit(min_expected_profit);
                    
                    return strategy;
                };
                
                auto strategy = retryOperation("Create CrossExchangePerpStrategy for " + 
                                             valid_exchanges[i]->getName() + " and " + 
                                             valid_exchanges[j]->getName(), create_strategy);
                
                strategies.push_back(std::move(strategy));
                logStrategyFactory("Successfully created CrossExchangePerpStrategy for " + 
                                  valid_exchanges[i]->getName() + " and " + 
                                  valid_exchanges[j]->getName() + " with min_funding_rate=" + 
                                  std::to_string(min_funding_rate) + ", min_expected_profit=" + 
                                  std::to_string(min_expected_profit));
            } catch (const std::exception& e) {
                logStrategyFactory("Failed to create CrossExchangePerpStrategy for " + 
                                  valid_exchanges[i]->getName() + " and " + 
                                  valid_exchanges[j]->getName() + ": " + e.what(), true);
            }
        }
    }
    
    if (strategies.empty()) {
        logStrategyFactory("No valid CrossExchangePerpStrategy instances created", true);
        return nullptr;
    }
    
    // If only one strategy, return it directly
    if (strategies.size() == 1) {
        logStrategyFactory("Returning single CrossExchangePerpStrategy");
        return std::move(strategies[0]);
    }
    
    // Otherwise, create a composite strategy
    logStrategyFactory("Creating composite strategy with " + 
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
    
    // Track exchanges that support spot and perpetual markets
    std::vector<std::shared_ptr<ExchangeInterface>> spot_exchanges;
    std::vector<std::shared_ptr<ExchangeInterface>> perp_exchanges;
    
    for (const auto& exchange : exchanges) {
        try {
            auto spot_instruments = exchange->getAvailableInstruments(MarketType::SPOT);
            if (!spot_instruments.empty()) {
                spot_exchanges.push_back(exchange);
            }
        } catch (const std::exception& e) {
            logStrategyFactory("Error checking spot instruments for " + 
                              exchange->getName() + ": " + e.what(), true);
        }
        
        try {
            auto perp_instruments = exchange->getAvailableInstruments(MarketType::PERPETUAL);
            if (!perp_instruments.empty()) {
                perp_exchanges.push_back(exchange);
            }
        } catch (const std::exception& e) {
            logStrategyFactory("Error checking perpetual instruments for " + 
                              exchange->getName() + ": " + e.what(), true);
        }
    }
    
    if (spot_exchanges.empty()) {
        logStrategyFactory("No exchanges with spot markets found for CrossExchangeSpotPerpStrategy", true);
        return nullptr;
    }
    
    if (perp_exchanges.empty()) {
        logStrategyFactory("No exchanges with perpetual markets found for CrossExchangeSpotPerpStrategy", true);
        return nullptr;
    }
    
    // Create strategies for all pairs of spot and perp exchanges
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (const auto& spot_exchange : spot_exchanges) {
        for (const auto& perp_exchange : perp_exchanges) {
            if (spot_exchange->getName() != perp_exchange->getName()) {
                try {
                    auto create_strategy = [&]() {
                        auto strategy = std::make_unique<CrossExchangeSpotPerpStrategy>(
                            spot_exchange, perp_exchange);
                        
                        // Configure strategy parameters
                        strategy->setMinFundingRate(min_funding_rate);
                        strategy->setMinExpectedProfit(min_expected_profit);
                        
                        return strategy;
                    };
                    
                    auto strategy = retryOperation("Create CrossExchangeSpotPerpStrategy for " + 
                                                 spot_exchange->getName() + " (spot) and " + 
                                                 perp_exchange->getName() + " (perp)", create_strategy);
                    
                    strategies.push_back(std::move(strategy));
                    logStrategyFactory("Successfully created CrossExchangeSpotPerpStrategy for " + 
                                      spot_exchange->getName() + " (spot) and " + 
                                      perp_exchange->getName() + " (perp) with min_funding_rate=" + 
                                      std::to_string(min_funding_rate) + ", min_expected_profit=" + 
                                      std::to_string(min_expected_profit));
                } catch (const std::exception& e) {
                    logStrategyFactory("Failed to create CrossExchangeSpotPerpStrategy for " + 
                                      spot_exchange->getName() + " and " + 
                                      perp_exchange->getName() + ": " + e.what(), true);
                }
            }
        }
    }
    
    if (strategies.empty()) {
        logStrategyFactory("No valid CrossExchangeSpotPerpStrategy instances created", true);
        return nullptr;
    }
    
    // If only one strategy, return it directly
    if (strategies.size() == 1) {
        logStrategyFactory("Returning single CrossExchangeSpotPerpStrategy");
        return std::move(strategies[0]);
    }
    
    // Otherwise, create a composite strategy
    logStrategyFactory("Creating composite strategy with " + 
                      std::to_string(strategies.size()) + " CrossExchangeSpotPerpStrategy instances");
    return std::make_unique<CompositeStrategy>(std::move(strategies));
}

// Creates a specific strategy instance based on provided configuration
std::unique_ptr<ArbitrageStrategy> createStrategy(
    const std::string& strategy_type,
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate,
    double min_expected_profit) {
    
    logStrategyFactory("Creating strategy of type '" + strategy_type + "' with min_funding_rate=" + 
                      std::to_string(min_funding_rate) + ", min_expected_profit=" + 
                      std::to_string(min_expected_profit));
    
    if (strategy_type == "same_exchange_spot_perp") {
        return createSameExchangeSpotPerpStrategy(exchanges, min_funding_rate, min_expected_profit);
    } else if (strategy_type == "cross_exchange_perp") {
        return createCrossExchangePerpStrategy(exchanges, min_funding_rate, min_expected_profit);
    } else if (strategy_type == "cross_exchange_spot_perp") {
        return createCrossExchangeSpotPerpStrategy(exchanges, min_funding_rate, min_expected_profit);
    } else {
        logStrategyFactory("Unknown strategy type: " + strategy_type, true);
        return nullptr;
    }
}

// Helper function to create all strategies from the configuration
std::vector<std::unique_ptr<ArbitrageStrategy>> createAllStrategies(
    const BotConfig& config,
    const std::map<std::string, std::shared_ptr<ExchangeInterface>>& exchanges) {
    
    logStrategyFactory("Creating all strategies from configuration with " + 
                      std::to_string(exchanges.size()) + " exchanges and " +
                      std::to_string(config.strategies.size()) + " strategy configs");
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    // Convert exchanges map to vector for strategy creation
    std::vector<std::shared_ptr<ExchangeInterface>> exchange_vec;
    for (const auto& [name, exchange] : exchanges) {
        exchange_vec.push_back(exchange);
    }
    
    // Create strategies based on configuration
    for (const auto& strategy_config : config.strategies) {
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
                logStrategyFactory("Unknown strategy type enum value: " + 
                                  std::to_string(static_cast<int>(strategy_config.type)), true);
                continue;
        }
        
        // Create strategy with configuration parameters
        auto strategy = createStrategy(
            strategy_type, 
            exchange_vec, 
            strategy_config.min_funding_rate,
            strategy_config.min_expected_profit
        );
        
        if (strategy) {
            strategies.push_back(std::move(strategy));
            logStrategyFactory("Added " + strategy_type + " strategy to the bot with min_funding_rate=" + 
                              std::to_string(strategy_config.min_funding_rate) + ", min_expected_profit=" + 
                              std::to_string(strategy_config.min_expected_profit));
        } else {
            logStrategyFactory("Failed to create " + strategy_type + " strategy", true);
        }
    }
    
    logStrategyFactory("Created " + std::to_string(strategies.size()) + " strategies in total");
    return strategies;
}

} // namespace funding 