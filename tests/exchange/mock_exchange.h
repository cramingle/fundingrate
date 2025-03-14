#pragma once

#include <exchange/exchange_interface.h>
#include <exchange/types.h>
#include <gmock/gmock.h>
#include <map>
#include <vector>
#include <string>

namespace funding {
namespace testing {

class MockExchange : public ExchangeInterface {
public:
    MOCK_METHOD(std::string, getName, (), (const, override));
    MOCK_METHOD(std::string, getBaseUrl, (), (const, override));
    
    // Market data methods
    MOCK_METHOD(std::vector<Instrument>, getAvailableInstruments, (MarketType), (override));
    MOCK_METHOD(double, getPrice, (const std::string&), (override));
    MOCK_METHOD(OrderBook, getOrderBook, (const std::string&, int), (override));
    MOCK_METHOD(FundingRate, getFundingRate, (const std::string&), (override));
    
    // Fee information methods
    MOCK_METHOD(FeeStructure, getFeeStructure, (), (override));
    MOCK_METHOD(double, getTradingFee, (const std::string&, bool), (override));
    MOCK_METHOD(double, getWithdrawalFee, (const std::string&, double), (override));
    
    // Account information methods
    MOCK_METHOD(AccountBalance, getAccountBalance, (), (override));
    MOCK_METHOD(std::vector<Position>, getOpenPositions, (), (override));
    
    // Trading operation methods
    MOCK_METHOD(std::string, placeOrder, (const Order&), (override));
    MOCK_METHOD(bool, cancelOrder, (const std::string&), (override));
    MOCK_METHOD(OrderStatus, getOrderStatus, (const std::string&), (override));
    
    // Historical data methods
    MOCK_METHOD(std::vector<Trade>, getRecentTrades, (const std::string&, int), (override));
    MOCK_METHOD(std::vector<Candle>, getCandles, 
               (const std::string&, const std::string&, 
                const std::chrono::system_clock::time_point&, 
                const std::chrono::system_clock::time_point&), (override));
    
    // Utility methods
    MOCK_METHOD(bool, isConnected, (), (override));
    MOCK_METHOD(bool, reconnect, (), (override));
    
    // Helper methods for setting up test scenarios
    void setupFundingRate(const std::string& symbol, double rate, 
                        std::chrono::hours interval = std::chrono::hours(8)) {
        FundingRate fr;
        fr.symbol = symbol;
        fr.rate = rate;
        fr.payment_interval = interval;
        fr.next_payment = std::chrono::system_clock::now() + interval;
        
        ON_CALL(*this, getFundingRate(symbol))
            .WillByDefault(::testing::Return(fr));
    }
    
    void setupPrice(const std::string& symbol, double price) {
        ON_CALL(*this, getPrice(symbol))
            .WillByDefault(::testing::Return(price));
    }
    
    void setupOrderBook(const std::string& symbol, const OrderBook& book) {
        ON_CALL(*this, getOrderBook(symbol, ::testing::_))
            .WillByDefault(::testing::Return(book));
    }
    
    void setupInstruments(MarketType type, const std::vector<Instrument>& instruments) {
        ON_CALL(*this, getAvailableInstruments(type))
            .WillByDefault(::testing::Return(instruments));
    }
    
    void setupFeeStructure(const FeeStructure& structure) {
        ON_CALL(*this, getFeeStructure())
            .WillByDefault(::testing::Return(structure));
    }
};

} // namespace testing
} // namespace funding 