#include <strategy/arbitrage_strategy.h>
#include <strategy/composite_strategy.h>
#include <config/config_manager.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <map>
#include <set>

namespace funding {

// Create a composite strategy for all same-exchange spot-perp opportunities
std::unique_ptr<ArbitrageStrategy> createSameExchangeSpotPerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges) {
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (const auto& exchange : exchanges) {
        strategies.push_back(std::make_unique<SameExchangeSpotPerpStrategy>(exchange));
    }
    
    if (strategies.empty()) {
        return nullptr;
    }
    
    return std::make_unique<CompositeStrategy>(std::move(strategies));
}

// Create a composite strategy for all cross-exchange perp-perp opportunities
std::unique_ptr<ArbitrageStrategy> createCrossExchangePerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges) {
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (size_t i = 0; i < exchanges.size(); ++i) {
        for (size_t j = i + 1; j < exchanges.size(); ++j) {
            strategies.push_back(std::make_unique<CrossExchangePerpStrategy>(
                exchanges[i], exchanges[j]));
        }
    }
    
    if (strategies.empty()) {
        return nullptr;
    }
    
    return std::make_unique<CompositeStrategy>(std::move(strategies));
}

// Create a composite strategy for all cross-exchange spot-perp opportunities
std::unique_ptr<ArbitrageStrategy> createCrossExchangeSpotPerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges) {
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    
    for (size_t i = 0; i < exchanges.size(); ++i) {
        for (size_t j = 0; j < exchanges.size(); ++j) {
            if (i != j) {
                strategies.push_back(std::make_unique<CrossExchangeSpotPerpStrategy>(
                    exchanges[i], exchanges[j]));
            }
        }
    }
    
    if (strategies.empty()) {
        return nullptr;
    }
    
    return std::make_unique<CompositeStrategy>(std::move(strategies));
}

// Creates a specific strategy instance based on provided configuration
std::unique_ptr<ArbitrageStrategy> createStrategy(
    const std::string& strategy_type,
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges) {
    
    if (strategy_type == "SAME_EXCHANGE_SPOT_PERP") {
        return createSameExchangeSpotPerpStrategy(exchanges);
    } 
    else if (strategy_type == "CROSS_EXCHANGE_PERP_PERP") {
        return createCrossExchangePerpStrategy(exchanges);
    } 
    else if (strategy_type == "CROSS_EXCHANGE_SPOT_PERP") {
        return createCrossExchangeSpotPerpStrategy(exchanges);
    }
    
    std::cerr << "Unknown strategy type: " << strategy_type << std::endl;
    return nullptr;
}

// Helper function to create all strategies from the configuration
std::vector<std::unique_ptr<ArbitrageStrategy>> createAllStrategies(
    const BotConfig& config,
    const std::map<std::string, std::shared_ptr<ExchangeInterface>>& exchanges) {
    
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies;
    std::vector<std::shared_ptr<ExchangeInterface>> exchange_vector;
    
    // Convert exchange map to vector
    for (const auto& [name, exchange] : exchanges) {
        exchange_vector.push_back(exchange);
    }
    
    // Create strategies for each strategy type in config
    for (const auto& strategy_config : config.strategies) {
        std::string strategy_type;
        
        // Convert strategy type enum to string
        switch (strategy_config.type) {
            case StrategyType::SAME_EXCHANGE_SPOT_PERP:
                strategy_type = "SAME_EXCHANGE_SPOT_PERP";
                break;
            case StrategyType::CROSS_EXCHANGE_PERP_PERP:
                strategy_type = "CROSS_EXCHANGE_PERP_PERP";
                break;
            case StrategyType::CROSS_EXCHANGE_SPOT_PERP:
                strategy_type = "CROSS_EXCHANGE_SPOT_PERP";
                break;
            default:
                std::cerr << "Unknown strategy type" << std::endl;
                continue;
        }
        
        // Create strategy
        auto strategy = createStrategy(strategy_type, exchange_vector);
        if (strategy) {
            strategies.push_back(std::move(strategy));
        }
    }
    
    return strategies;
}

} // namespace funding 