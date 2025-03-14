#pragma once

#include <memory>
#include <vector>
#include <map>
#include <string>
#include <strategy/arbitrage_strategy.h>
#include <config/config_manager.h>

namespace funding {

// Create a strategy for same exchange spot-perp arbitrage
std::unique_ptr<ArbitrageStrategy> createSameExchangeSpotPerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate = 0.0001,
    double min_expected_profit = 1.0);

// Create a strategy for cross-exchange perp-perp arbitrage
std::unique_ptr<ArbitrageStrategy> createCrossExchangePerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate = 0.0001,
    double min_expected_profit = 1.0);

// Create a strategy for cross-exchange spot-perp arbitrage
std::unique_ptr<ArbitrageStrategy> createCrossExchangeSpotPerpStrategy(
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate = 0.0001,
    double min_expected_profit = 1.0);

// Create a strategy based on type string
std::unique_ptr<ArbitrageStrategy> createStrategy(
    const std::string& strategy_type,
    const std::vector<std::shared_ptr<ExchangeInterface>>& exchanges,
    double min_funding_rate = 0.0001,
    double min_expected_profit = 1.0);

// Create all strategies based on configuration
std::vector<std::unique_ptr<ArbitrageStrategy>> createAllStrategies(
    const BotConfig& config,
    const std::map<std::string, std::shared_ptr<ExchangeInterface>>& exchanges);

} // namespace funding 