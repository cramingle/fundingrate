#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <unordered_map>
#include "types.h"

namespace funding {

// Forward declarations
class ExchangeConfig;

// Interface for exchange API interactions
class ExchangeInterface {
public:
    virtual ~ExchangeInterface() = default;
    
    // Exchange information
    virtual std::string getName() const = 0;
    virtual std::string getBaseUrl() const = 0;
    
    // Market data
    virtual std::vector<Instrument> getAvailableInstruments(MarketType type) = 0;
    virtual double getPrice(const std::string& symbol) = 0;
    virtual OrderBook getOrderBook(const std::string& symbol, int depth = 10) = 0;
    virtual FundingRate getFundingRate(const std::string& symbol) = 0;
    
    // Fee information
    virtual FeeStructure getFeeStructure() = 0;
    virtual double getTradingFee(const std::string& symbol, bool is_maker = false) = 0;
    virtual double getWithdrawalFee(const std::string& currency, double amount = 0.0) = 0;
    
    // Account information
    virtual AccountBalance getAccountBalance() = 0;
    virtual std::vector<Position> getOpenPositions() = 0;
    
    // Trading operations
    virtual std::string placeOrder(const Order& order) = 0;
    virtual bool cancelOrder(const std::string& order_id) = 0;
    virtual OrderStatus getOrderStatus(const std::string& order_id) = 0;
    
    // Historical data
    virtual std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) = 0;
    virtual std::vector<Candle> getCandles(const std::string& symbol, 
                                         const std::string& interval,
                                         const std::chrono::system_clock::time_point& start,
                                         const std::chrono::system_clock::time_point& end) = 0;
    
    // Utility functions
    virtual bool isConnected() = 0;
    virtual bool reconnect() = 0;
};

// Factory function to create exchange interfaces
std::shared_ptr<ExchangeInterface> createExchangeInterface(const ExchangeConfig& config);

} // namespace funding