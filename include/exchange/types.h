#pragma once

#include <vector>
#include <string>
#include <chrono>
#include <map>
#include <unordered_map>

namespace funding {

// Forward declare MarketType enum to avoid circular include
enum class MarketType { SPOT, MARGIN, PERPETUAL };

// Order types and side
enum class OrderSide { BUY, SELL };
enum class OrderType { MARKET, LIMIT };

// Order status enum
enum class OrderStatus {
    NEW,
    PARTIALLY_FILLED, 
    FILLED,
    CANCELED,
    REJECTED,
    EXPIRED
};

// Represents a price level in the order book
struct PriceLevel {
    double price;
    double amount;
};

// Alias for OrderBookLevel to maintain compatibility with implementation
using OrderBookLevel = PriceLevel;

// Represents the order book for a specific symbol
struct OrderBook {
    std::string symbol;
    std::vector<PriceLevel> bids;  // Sorted by price descending
    std::vector<PriceLevel> asks;  // Sorted by price ascending
    std::chrono::system_clock::time_point timestamp;
};

// Represents market data snapshot
struct MarketData {
    std::string symbol;
    double last_price;
    double high_24h;
    double low_24h;
    double volume_24h;
    double price_change_percent_24h;
    std::chrono::system_clock::time_point timestamp;
};

// Represents a trade
struct Trade {
    std::string symbol;
    double price;
    double amount;
    std::string side;  // "buy" or "sell"
    std::chrono::system_clock::time_point timestamp;
    std::string trade_id;
};

// Represents a candle/kline
struct Candle {
    std::string symbol;
    std::chrono::system_clock::time_point open_time;
    std::chrono::system_clock::time_point close_time;
    double open;
    double high;
    double low;
    double close;
    double volume;
    int trades;
};

// Account balance information
struct AccountBalance {
    std::map<std::string, double> total;      // Total balance by currency
    std::map<std::string, double> available;  // Available balance by currency
    std::map<std::string, double> locked;     // Locked/used balance by currency
};

// Position information
struct Position {
    std::string symbol;
    double size;                // Positive for long, negative for short
    double entry_price;
    double liquidation_price;
    double unrealized_pnl;
    double leverage;
};

// Order information
struct Order {
    std::string order_id;
    std::string symbol;
    OrderSide side;
    OrderType type;
    double price;
    double amount;
    double filled;
    std::chrono::system_clock::time_point created_at;
    std::string status;
};

// Fee structure for an exchange
struct FeeStructure {
    // General trading fees (default fees)
    double maker_fee;       // Fee for making liquidity (limit orders that don't cross the spread)
    double taker_fee;       // Fee for taking liquidity (market orders or crossing the spread)
    
    // Different market type fees (if applicable)
    double spot_maker_fee;
    double spot_taker_fee;
    double perp_maker_fee;
    double perp_taker_fee;
    double margin_maker_fee;
    double margin_taker_fee;
    
    // VIP tier information
    int fee_tier;
    std::string tier_name;
    double volume_30d_usd;  // 30-day trading volume in USD
    
    // Withdrawal fees
    std::unordered_map<std::string, double> withdrawal_fees;
    
    // Fee discount information
    double fee_discount_pct;
    bool has_token_discount;  // Whether exchange token provides fee discount
    
    FeeStructure() : 
        maker_fee(0.0001),     // Default to 0.01%
        taker_fee(0.0005),     // Default to 0.05%
        spot_maker_fee(0.0001),
        spot_taker_fee(0.0005),
        perp_maker_fee(0.0001),
        perp_taker_fee(0.0005),
        margin_maker_fee(0.0001),
        margin_taker_fee(0.0005),
        fee_tier(0),
        tier_name("Default"),
        volume_30d_usd(0.0),
        fee_discount_pct(0.0),
        has_token_discount(false) {}
};

// Trading pair for arbitrage
struct TradingPair {
    std::string exchange1;
    std::string symbol1;
    MarketType market_type1;
    
    std::string exchange2;
    std::string symbol2;
    MarketType market_type2;
    
    // Default constructor
    TradingPair() = default;
    
    // Constructor for same exchange, different market types
    TradingPair(const std::string& exchange,
                const std::string& symbol, 
                MarketType spot_or_margin_type,
                MarketType perp_type) :
        exchange1(exchange), 
        symbol1(symbol),
        market_type1(spot_or_margin_type),
        exchange2(exchange),
        symbol2(symbol),
        market_type2(perp_type) {}
    
    // Constructor for different exchanges
    TradingPair(const std::string& exchange1_name, 
                const std::string& symbol1_name,
                MarketType market_type1_val,
                const std::string& exchange2_name,
                const std::string& symbol2_name,
                MarketType market_type2_val) :
        exchange1(exchange1_name),
        symbol1(symbol1_name),
        market_type1(market_type1_val),
        exchange2(exchange2_name),
        symbol2(symbol2_name),
        market_type2(market_type2_val) {}
};

// Represents a trading instrument
struct Instrument {
    std::string symbol;          // e.g. "BTC/USDT"
    MarketType market_type;      // SPOT, MARGIN or PERPETUAL
    std::string base_currency;   // e.g. "BTC"
    std::string quote_currency;  // e.g. "USDT" 
    double min_order_size;       // Minimum order size
    double min_qty;              // Alias for min_order_size for compatibility
    double max_qty;              // Maximum order size
    double tick_size;            // Minimum price movement
    double price_precision;      // Decimal places for price
    double qty_precision;        // Decimal places for quantity
};

// Funding rate information
struct FundingRate {
    std::string symbol;
    double rate;                                  // Current funding rate
    std::chrono::system_clock::time_point next_payment;   // Next funding time
    std::chrono::hours payment_interval;          // Funding interval (1h, 4h, 8h)
    double predicted_rate;                        // Predicted next funding rate
};

// Represents an arbitrage opportunity
struct ArbitrageOpportunity {
    TradingPair pair;
    double funding_rate1;
    double funding_rate2;
    double net_funding_rate;         // Annualized expected return from funding
    std::chrono::hours payment_interval1;
    std::chrono::hours payment_interval2;
    double entry_price_spread;       // Current price difference
    double max_allowable_spread;     // Maximum spread before opportunity becomes unprofitable
    double transaction_cost_pct;     // Total transaction cost as percentage (entry + exit)
    double estimated_profit;         // Estimated profit per unit size
    double periods_to_breakeven;     // Number of funding periods to break even on transaction costs
    double max_position_size;        // Maximum position size based on liquidity
    double position_risk_score;      // Risk score (0-100)
    std::chrono::system_clock::time_point discovery_time;
    std::string strategy_type;       // Identifier for the strategy that created this opportunity
    int strategy_index;              // Index of the strategy in the composite strategy (if applicable)
};

} // namespace funding 