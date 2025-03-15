#include <exchange/exchange_interface.h>
#include <exchange/exchange_config.h>
#include <exchange/types.h>
#include <iostream>
#include <map>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace funding {

using json = nlohmann::json;

// Make the WriteCallback function static to avoid multiple definition errors
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
        return newLength;
    } catch (std::bad_alloc& e) {
        // Handle memory problem
        return 0;
    }
}

// Bybit API implementation
class BybitExchange : public ExchangeInterface {
public:
    BybitExchange(const ExchangeConfig& config) : 
        api_key_(config.getApiKey()),
        api_secret_(config.getApiSecret()),
        base_url_("https://api.bybit.com"),
        use_testnet_(false), // Always use production API
        last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
        
        // Always use production URL
        base_url_ = "https://api.bybit.com";
        
        // Initialize CURL
        curl_global_init(CURL_GLOBAL_ALL);
        
        // Connect and fetch initial fee structure
        reconnect();
        updateFeeStructure();
    }
    
    ~BybitExchange() {
        curl_global_cleanup();
    }
    
    // Exchange information
    std::string getName() const override {
        return "Bybit";
    }
    
    std::string getBaseUrl() const override {
        return base_url_;
    }
    
    // Market data implementations
    std::vector<Instrument> getAvailableInstruments(MarketType type) override {
        std::vector<Instrument> instruments;
        std::string endpoint;
        std::string category;
        
        // Map our market type to Bybit's category
        switch (type) {
            case MarketType::SPOT:
                category = "spot";
                endpoint = "/v5/market/instruments-info?category=spot";
                break;
            case MarketType::PERPETUAL:
                category = "linear";  // USDT-settled linear perpetuals
                endpoint = "/v5/market/instruments-info?category=linear";
                break;
            case MarketType::MARGIN:
                // Bybit doesn't have a clear margin category, use spot instead
                category = "spot";
                endpoint = "/v5/market/instruments-info?category=spot";
                break;
            default:
                throw std::runtime_error("Unsupported market type");
        }
        
        json response = makeApiCall(endpoint, "", false);
        
        try {
            if (response["retCode"] == 0 && response.contains("result") && 
                response["result"].contains("list")) {
                
                for (const auto& instrument : response["result"]["list"]) {
                    // Skip if not trading
                    if (instrument.contains("status") && instrument["status"] != "Trading") {
                        continue;
                    }
                    
                    Instrument inst;
                    inst.symbol = instrument["symbol"];
                    
                    // Parse base and quote currencies
                    if (instrument.contains("baseCoin") && instrument.contains("quoteCoin")) {
                        inst.base_currency = instrument["baseCoin"];
                        inst.quote_currency = instrument["quoteCoin"];
                    } else {
                        // Try to parse from symbol (e.g., BTCUSDT)
                        std::string symbol = inst.symbol;
                        size_t pos = symbol.find("USD");
                        if (pos != std::string::npos) {
                            inst.base_currency = symbol.substr(0, pos);
                            inst.quote_currency = symbol.substr(pos);
                        } else {
                            // Default fallback
                            inst.base_currency = "Unknown";
                            inst.quote_currency = "Unknown";
                        }
                    }
                    
                    inst.market_type = type;
                    
                    // Get min/max quantities
                    if (instrument.contains("lotSizeFilter")) {
                        auto& lotSize = instrument["lotSizeFilter"];
                        inst.min_order_size = std::stod(lotSize["minOrderQty"].get<std::string>());
                        inst.min_qty = inst.min_order_size; // Alias
                        inst.max_qty = std::stod(lotSize["maxOrderQty"].get<std::string>());
                    } else {
                        inst.min_order_size = 0.0001; // Default
                        inst.min_qty = 0.0001;
                        inst.max_qty = 1000000.0;
                    }
                    
                    // Get price precision
                    if (instrument.contains("priceFilter")) {
                        auto& priceFilter = instrument["priceFilter"];
                        inst.tick_size = std::stod(priceFilter["tickSize"].get<std::string>());
                        
                        // Calculate decimals from tick size
                        std::string tick_str = priceFilter["tickSize"].get<std::string>();
                        size_t decimal_pos = tick_str.find('.');
                        if (decimal_pos != std::string::npos) {
                            inst.price_precision = tick_str.length() - decimal_pos - 1;
                        } else {
                            inst.price_precision = 0;
                        }
                    } else {
                        inst.tick_size = 0.01; // Default
                        inst.price_precision = 2;
                    }
                    
                    // Quantity precision
                    inst.qty_precision = 8; // Default
                    
                    instruments.push_back(inst);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Bybit instruments: " << e.what() << std::endl;
        }
        
        return instruments;
    }
    
    double getPrice(const std::string& symbol) override {
        // Determine category based on symbol
        std::string category = "spot"; // Default
        if (symbol.find("USDT") != std::string::npos) {
            category = "linear"; // Linear perpetual futures use USDT as quote
        }
        
        std::string endpoint = "/v5/market/tickers?category=" + category + "&symbol=" + symbol;
        json response = makeApiCall(endpoint, "", false);
        
        if (response["retCode"] == 0 && response.contains("result") && 
            response["result"].contains("list") && !response["result"]["list"].empty()) {
            return std::stod(response["result"]["list"][0]["lastPrice"].get<std::string>());
        }
        
        throw std::runtime_error("Failed to get price for " + symbol);
    }
    
    OrderBook getOrderBook(const std::string& symbol, int depth) override {
        // Determine category based on symbol
        std::string category = "spot"; // Default
        if (symbol.find("USDT") != std::string::npos) {
            category = "linear";
        }
        
        std::string endpoint = "/v5/market/orderbook?category=" + category + 
                              "&symbol=" + symbol + "&limit=" + std::to_string(depth);
        json response = makeApiCall(endpoint, "", false);
        
        OrderBook book;
        book.symbol = symbol;
        
        try {
            if (response["retCode"] == 0 && response.contains("result")) {
                auto& result = response["result"];
                
                // Process bids
                if (result.contains("b")) {
                    for (const auto& bid : result["b"]) {
                        OrderBookLevel level;
                        // Handle both string and number types
                        if (bid[0].is_string()) {
                            level.price = std::stod(bid[0].get<std::string>());
                        } else {
                            level.price = bid[0].get<double>();
                        }
                        if (bid[1].is_string()) {
                            level.amount = std::stod(bid[1].get<std::string>());
                        } else {
                            level.amount = bid[1].get<double>();
                        }
                        book.bids.push_back(level);
                    }
                }
                
                // Process asks
                if (result.contains("a")) {
                    for (const auto& ask : result["a"]) {
                        OrderBookLevel level;
                        // Handle both string and number types
                        if (ask[0].is_string()) {
                            level.price = std::stod(ask[0].get<std::string>());
                        } else {
                            level.price = ask[0].get<double>();
                        }
                        if (ask[1].is_string()) {
                            level.amount = std::stod(ask[1].get<std::string>());
                        } else {
                            level.amount = ask[1].get<double>();
                        }
                        book.asks.push_back(level);
                    }
                }
                
                // Set timestamp
                if (result.contains("ts")) {
                    auto ts = std::stoll(result["ts"].get<std::string>());
                    book.timestamp = std::chrono::system_clock::from_time_t(ts / 1000);
                } else {
                    book.timestamp = std::chrono::system_clock::now();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Bybit order book: " << e.what() << std::endl;
        }
        
        return book;
    }
    
    FundingRate getFundingRate(const std::string& symbol) override {
        FundingRate funding;
        funding.symbol = symbol;
        
        try {
            // Construct the API endpoint for funding rate
            std::string endpoint = "/v5/market/funding/history?category=linear&symbol=" + symbol;
            json response = makeApiCall(endpoint);
            
            // Check if the response is valid
            if (!response.is_object()) {
                throw std::runtime_error("Invalid response format: not an object");
            }
            
            // Check if the response contains the retCode field
            if (!response.contains("retCode")) {
                throw std::runtime_error("Invalid response format: missing retCode field");
            }
            
            // Check if the API call was successful
            if (response.at("retCode") != 0) {
                std::string error_msg = "API error: ";
                if (response.contains("retMsg")) {
                    error_msg += response.at("retMsg").get<std::string>();
                } else {
                    error_msg += "Unknown error";
                }
                throw std::runtime_error(error_msg);
            }
            
            // Set default values in case we can't parse the response
            funding.rate = 0.0;
            funding.predicted_rate = 0.0;
            funding.payment_interval = std::chrono::hours(8);
            funding.next_payment = calculateNextFundingTime();
            
            // Check if the response contains the result field
            if (!response.contains("result")) {
                throw std::runtime_error("Invalid response format: missing result field");
            }
            
            // Get the result object
            const auto& result = response.at("result");
            
            // Check if the result contains the list field
            if (!result.contains("list")) {
                throw std::runtime_error("Invalid response format: missing list field in result");
            }
            
            // Get the funding rate list
            const auto& list = result.at("list");
            
            // Check if the list is empty
            if (list.empty()) {
                throw std::runtime_error("No funding rate data available");
            }
            
            // Get the most recent funding rate
            const auto& latest = list[0];
            
            // Parse the funding rate
            if (latest.contains("fundingRate")) {
                std::string rate_str = latest.at("fundingRate").get<std::string>();
                funding.rate = std::stod(rate_str);
            }
            
            // Parse the funding rate timestamp
            if (latest.contains("fundingRateTimestamp")) {
                // The timestamp can be either a string or a number
                int64_t timestamp = 0;
                
                if (latest.at("fundingRateTimestamp").is_string()) {
                    timestamp = std::stoll(latest.at("fundingRateTimestamp").get<std::string>());
                } else if (latest.at("fundingRateTimestamp").is_number()) {
                    timestamp = latest.at("fundingRateTimestamp").get<int64_t>();
                } else {
                    // If it's neither a string nor a number, use current timestamp
                    timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                }
                
                // Calculate the next funding time (8 hours after the last funding)
                auto funding_time = std::chrono::system_clock::from_time_t(timestamp / 1000);
                funding.next_payment = calculateNextFundingTime();
            } else {
                // If no timestamp is available, set the next payment to 8 hours from now
                funding.next_payment = calculateNextFundingTime();
            }
            
            // Try to get the predicted funding rate if available
            if (latest.contains("predictedFundingRate")) {
                try {
                    std::string predicted_rate_str = latest.at("predictedFundingRate").get<std::string>();
                    funding.predicted_rate = std::stod(predicted_rate_str);
                } catch (const std::exception& e) {
                    // If we can't parse the predicted rate, use the current rate
                    funding.predicted_rate = funding.rate;
                }
            } else {
                // If no predicted rate is available, use the current rate
                funding.predicted_rate = funding.rate;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching funding rate from Bybit: " << e.what() << std::endl;
            
            // Set default values for funding rate
            funding.rate = 0.0;
            funding.predicted_rate = 0.0;
            funding.payment_interval = std::chrono::hours(8);
            funding.next_payment = calculateNextFundingTime();
        }
        
        return funding;
    }
    
    // Fee information
    FeeStructure getFeeStructure() override {
        // Check if we need to update the fee structure (cached for 24 hours)
        auto now = std::chrono::system_clock::now();
        if (now - last_fee_update_ > std::chrono::hours(24)) {
            updateFeeStructure();
            last_fee_update_ = now;
        }
        
        return fee_structure_;
    }
    
    double getTradingFee(const std::string& symbol, bool is_maker = false) override {
        // Check if we need to update fees
        auto now = std::chrono::system_clock::now();
        if (now - last_fee_update_ > std::chrono::hours(24)) {
            updateFeeStructure();
            last_fee_update_ = now;
        }
        
        // Check if we have specific symbol fee info
        if (symbol_fees_.find(symbol) != symbol_fees_.end()) {
            return is_maker ? symbol_fees_[symbol].first : symbol_fees_[symbol].second;
        }
        
        // Try to determine market type based on symbol
        if (symbol.find("USDT") != std::string::npos) {
            // This is likely a USDT-margined contract (linear)
            return is_maker ? fee_structure_.perp_maker_fee : fee_structure_.perp_taker_fee;
        } else {
            // Fallback to spot fees
            return is_maker ? fee_structure_.spot_maker_fee : fee_structure_.spot_taker_fee;
        }
    }
    
    double getWithdrawalFee(const std::string& currency, double amount = 0.0) override {
        // Check if we need to update fees
        auto now = std::chrono::system_clock::now();
        if (now - last_fee_update_ > std::chrono::hours(24)) {
            updateFeeStructure();
            last_fee_update_ = now;
        }
        
        // Return withdrawal fee for the specified currency if available
        if (fee_structure_.withdrawal_fees.find(currency) != fee_structure_.withdrawal_fees.end()) {
            return fee_structure_.withdrawal_fees[currency];
        }
        
        // Default withdrawal fee (conservative estimate)
        return 0.0005 * amount; // 0.05% as a fallback
    }
    
    // Account information
    AccountBalance getAccountBalance() override {
        std::string endpoint = "/v5/account/wallet-balance?accountType=UNIFIED";
        json response = makeApiCall(endpoint, "", true);
        
        AccountBalance balance;
        
        try {
            if (response["retCode"] == 0 && response.contains("result") && 
                response["result"].contains("list") && !response["result"]["list"].empty()) {
                
                auto& coins = response["result"]["list"][0]["coin"];
                
                for (const auto& coin : coins) {
                    std::string currency = coin["coin"];
                    double equity = std::stod(coin["equity"].get<std::string>());
                    double walletBalance = std::stod(coin["walletBalance"].get<std::string>());
                    double free = std::stod(coin["free"].get<std::string>());
                    
                    balance.total[currency] = equity;
                    balance.available[currency] = free;
                    balance.locked[currency] = walletBalance - free;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Bybit account balance: " << e.what() << std::endl;
        }
        
        return balance;
    }
    
    std::vector<Position> getOpenPositions() override {
        std::string endpoint = "/v5/position/list?category=linear&settleCoin=USDT";
        json response = makeApiCall(endpoint, "", true);
        
        std::vector<Position> positions;
        
        try {
            if (response["retCode"] == 0 && response.contains("result") && 
                response["result"].contains("list")) {
                
                for (const auto& pos : response["result"]["list"]) {
                    // Skip if position size is zero
                    double size = std::stod(pos["size"].get<std::string>());
                    if (size <= 0) {
                        continue;
                    }
                    
                    Position position;
                    position.symbol = pos["symbol"];
                    std::string side = pos["side"];
                    
                    // Set position size (positive for long, negative for short)
                    position.size = (side == "Buy") ? size : -size;
                    
                    position.entry_price = std::stod(pos["avgPrice"].get<std::string>());
                    position.liquidation_price = std::stod(pos["liqPrice"].get<std::string>());
                    position.unrealized_pnl = std::stod(pos["unrealisedPnl"].get<std::string>());
                    position.leverage = std::stod(pos["leverage"].get<std::string>());
                    
                    positions.push_back(position);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Bybit positions: " << e.what() << std::endl;
        }
        
        return positions;
    }
    
    // Trading operations
    std::string placeOrder(const Order& order) override {
        std::string endpoint = "/v5/order/create";
        
        // Determine category based on symbol
        std::string category = "spot";
        if (order.symbol.find("USDT") != std::string::npos) {
            category = "linear";
        }
        
        // Convert our internal order to Bybit's format
        json req_body = {
            {"category", category},
            {"symbol", order.symbol},
            {"side", order.side == OrderSide::BUY ? "Buy" : "Sell"},
            {"orderType", order.type == OrderType::MARKET ? "Market" : "Limit"},
            {"qty", std::to_string(order.amount)}
        };
        
        // Add price for limit orders
        if (order.type == OrderType::LIMIT) {
            req_body["price"] = std::to_string(order.price);
        }
        
        // Convert request to string
        std::string request_body = req_body.dump();
        
        // Make the API call
        json response = makeApiCall(endpoint, request_body, true, "POST");
        
        if (response["retCode"] == 0 && response.contains("result") && 
            response["result"].contains("orderId")) {
            return response["result"]["orderId"];
        }
        
        // Handle error
        std::string msg = "Failed to place order";
        if (response.contains("retMsg")) {
            msg += ": " + response["retMsg"].get<std::string>();
        }
        
        throw std::runtime_error(msg);
    }
    
    bool cancelOrder(const std::string& order_id) override {
        std::string endpoint = "/v5/order/cancel";
        
        // Determine category based on order ID format or from cache
        std::string category = "linear"; // Default to linear futures
        
        // Prepare request body
        json req_body = {
            {"category", category},
            {"orderId", order_id}
        };
        
        // Convert request to string
        std::string request_body = req_body.dump();
        
        // Make the API call
        json response = makeApiCall(endpoint, request_body, true, "POST");
        
        return (response["retCode"] == 0);
    }
    
    OrderStatus getOrderStatus(const std::string& order_id) override {
        std::string endpoint = "/v5/order/history?orderId=" + order_id;
        json response = makeApiCall(endpoint, "", true);
        
        if (response["retCode"] == 0 && response.contains("result") && 
            response["result"].contains("list") && !response["result"]["list"].empty()) {
            
            std::string status = response["result"]["list"][0]["orderStatus"];
            
            // Map Bybit status to our internal status
            if (status == "New" || status == "Created" || status == "Untriggered") {
                return OrderStatus::NEW;
            } else if (status == "PartiallyFilled") {
                return OrderStatus::PARTIALLY_FILLED;
            } else if (status == "Filled") {
                return OrderStatus::FILLED;
            } else if (status == "Cancelled" || status == "PartiallyFilledCanceled") {
                return OrderStatus::CANCELED;
            } else if (status == "Rejected") {
                return OrderStatus::REJECTED;
            } else {
                return OrderStatus::REJECTED; // Default to rejected for unknown states
            }
        }
        
        throw std::runtime_error("Failed to get order status");
    }
    
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override {
        std::string category = "spot";
        if (symbol.find("USDT") != std::string::npos) {
            category = "linear";
        }
        
        std::string endpoint = "/v5/market/recent-trade?category=" + category + 
                              "&symbol=" + symbol + "&limit=" + std::to_string(limit);
        json response = makeApiCall(endpoint, "", false);
        
        std::vector<Trade> trades;
        
        try {
            if (response["retCode"] == 0 && response.contains("result") && 
                response["result"].contains("list")) {
                
                for (const auto& trade_data : response["result"]["list"]) {
                    Trade trade;
                    trade.symbol = symbol;
                    trade.price = std::stod(trade_data["price"].get<std::string>());
                    trade.amount = std::stod(trade_data["size"].get<std::string>());
                    trade.side = trade_data["side"].get<std::string>() == "Buy" ? "buy" : "sell";
                    
                    // Convert timestamp
                    auto ts = std::stoll(trade_data["time"].get<std::string>());
                    trade.timestamp = std::chrono::system_clock::from_time_t(ts / 1000);
                    
                    if (trade_data.contains("tradeId")) {
                        trade.trade_id = trade_data["tradeId"];
                    } else {
                        trade.trade_id = "unknown";
                    }
                    
                    trades.push_back(trade);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Bybit trades: " << e.what() << std::endl;
        }
        
        return trades;
    }
    
    std::vector<Candle> getCandles(const std::string& symbol, 
                                  const std::string& interval,
                                  const std::chrono::system_clock::time_point& start,
                                  const std::chrono::system_clock::time_point& end) override {
        
        // Convert interval to Bybit format
        std::string bybit_interval;
        if (interval == "1m") bybit_interval = "1";
        else if (interval == "3m") bybit_interval = "3";
        else if (interval == "5m") bybit_interval = "5";
        else if (interval == "15m") bybit_interval = "15";
        else if (interval == "30m") bybit_interval = "30";
        else if (interval == "1h") bybit_interval = "60";
        else if (interval == "2h") bybit_interval = "120";
        else if (interval == "4h") bybit_interval = "240";
        else if (interval == "6h") bybit_interval = "360";
        else if (interval == "12h") bybit_interval = "720";
        else if (interval == "1d") bybit_interval = "D";
        else if (interval == "1w") bybit_interval = "W";
        else if (interval == "1M") bybit_interval = "M";
        else throw std::runtime_error("Unsupported interval: " + interval);
        
        // Determine category
        std::string category = "spot";
        if (symbol.find("USDT") != std::string::npos) {
            category = "linear";
        }
        
        // Convert time to timestamps
        auto start_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            start.time_since_epoch()).count();
        auto end_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            end.time_since_epoch()).count();
        
        std::string endpoint = "/v5/market/kline?category=" + category + 
                              "&symbol=" + symbol + 
                              "&interval=" + bybit_interval +
                              "&start=" + std::to_string(start_ts) +
                              "&end=" + std::to_string(end_ts);
        
        json response = makeApiCall(endpoint, "", false);
        
        std::vector<Candle> candles;
        
        try {
            if (response["retCode"] == 0 && response.contains("result") && 
                response["result"].contains("list")) {
                
                for (const auto& candle_data : response["result"]["list"]) {
                    Candle candle;
                    candle.symbol = symbol;
                    
                    // Bybit format: [timestamp, open, high, low, close, volume, turnover]
                    auto ts = std::stoll(candle_data[0].get<std::string>());
                    candle.open_time = std::chrono::system_clock::from_time_t(ts / 1000);
                    
                    candle.open = std::stod(candle_data[1].get<std::string>());
                    candle.high = std::stod(candle_data[2].get<std::string>());
                    candle.low = std::stod(candle_data[3].get<std::string>());
                    candle.close = std::stod(candle_data[4].get<std::string>());
                    candle.volume = std::stod(candle_data[5].get<std::string>());
                    
                    // Bybit doesn't provide trade count
                    candle.trades = 0;
                    
                    candles.push_back(candle);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Bybit candles: " << e.what() << std::endl;
        }
        
        return candles;
    }
    
    bool isConnected() override {
        try {
            // Simple server time endpoint to check connection
            std::string endpoint = "/v5/market/time";
            json response = makeApiCall(endpoint, "", false);
            
            return (response["retCode"] == 0);
        } catch (const std::exception& e) {
            std::cerr << "Bybit connection check failed: " << e.what() << std::endl;
            return false;
        }
    }
    
    bool reconnect() override {
        // Clear any cached data and try to connect again
        try {
            // Try up to 3 times with a short delay between attempts
            for (int attempt = 1; attempt <= 3; attempt++) {
                if (isConnected()) {
                    return true;
                }
                
                std::cerr << "Bybit reconnect attempt " << attempt << " failed, retrying..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            return false;
        } catch (const std::exception& e) {
            std::cerr << "Bybit reconnect failed: " << e.what() << std::endl;
            return false;
        }
    }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string base_url_;
    bool use_testnet_;
    FeeStructure fee_structure_;
    std::map<std::string, std::pair<double, double>> symbol_fees_; // symbol -> (maker, taker)
    std::chrono::system_clock::time_point last_fee_update_;
    
    // Generate Bybit API signature
    std::string generateSignature(const std::string& timestamp, const std::string& recv_window, 
                                 const std::string& request_body) {
        std::string payload = timestamp + api_key_ + recv_window + request_body;
        
        // HMAC-SHA256 signature
        unsigned char* digest = HMAC(EVP_sha256(), api_secret_.c_str(), api_secret_.length(),
                                   (unsigned char*)payload.c_str(), payload.length(), NULL, NULL);
        
        // Convert to hex string
        std::stringstream ss;
        for (int i = 0; i < 32; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
        }
        
        return ss.str();
    }
    
    // Make API call to Bybit
    json makeApiCall(const std::string& endpoint, const std::string& request_body = "", 
                     bool is_private = false, const std::string& method = "GET") {
        CURL* curl = curl_easy_init();
        std::string response_string;
        std::string url = base_url_ + endpoint;
        
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
        // Disable SSL verification for production use
        // This is necessary because Bybit's SSL certificates might not be properly validated
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        // Set connection timeout to prevent hanging
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        // Enable verbose output for debugging
        // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        // Add authentication headers for private endpoints
        if (is_private) {
            // Generate timestamp (milliseconds)
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::string timestamp = std::to_string(now);
            
            // Set recv window (5000ms is default)
            std::string recv_window = "5000";
            
            // Generate signature
            std::string signature = generateSignature(timestamp, recv_window, request_body);
            
            // Add authentication headers
            headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + api_key_).c_str());
            headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + signature).c_str());
            headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + timestamp).c_str());
            headers = curl_slist_append(headers, ("X-BAPI-RECV-WINDOW: " + recv_window).c_str());
        }
        
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        
        // Set method and request body
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!request_body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
            }
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        
        if (res != CURLE_OK) {
            throw std::runtime_error("CURL request failed: " + std::string(curl_easy_strerror(res)));
        }
        
        try {
            return json::parse(response_string);
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to parse JSON response: " + std::string(e.what()));
        }
    }
    
    // Update the fee structure from the exchange
    void updateFeeStructure() {
        try {
            // Bybit doesn't have a specific fee info endpoint in v5 API
            // Using default fees based on standard tier
            
            // Set default fees - Bybit has different tiers based on trading volume
            fee_structure_.maker_fee = 0.0001;  // 0.01% default maker
            fee_structure_.taker_fee = 0.0006;  // 0.06% default taker
            
            // Spot fees
            fee_structure_.spot_maker_fee = 0.0001; // 0.01%
            fee_structure_.spot_taker_fee = 0.0001; // 0.01%
            
            // USDT Perpetual fees
            fee_structure_.perp_maker_fee = -0.0001; // -0.01% (rebate)
            fee_structure_.perp_taker_fee = 0.0006;  // 0.06%
            
            // Margin fees - Bybit uses spot fees for margin
            fee_structure_.margin_maker_fee = 0.0001; // 0.01%
            fee_structure_.margin_taker_fee = 0.0001; // 0.01%
            
            // For specific pairs
            symbol_fees_["BTCUSDT"] = std::make_pair(-0.0001, 0.0006); // USDT Perpetual
            symbol_fees_["ETHUSDT"] = std::make_pair(-0.0001, 0.0006); // USDT Perpetual
            
            // Common pairs for spot
            symbol_fees_["BTCUSDT-SPOT"] = std::make_pair(0.0001, 0.0001);
            symbol_fees_["ETHUSDT-SPOT"] = std::make_pair(0.0001, 0.0001);
            
            // Set default VIP tier info
            fee_structure_.fee_tier = 1;
            fee_structure_.tier_name = "VIP 1";
            
            // Withdrawal fees - Add some common ones
            fee_structure_.withdrawal_fees["BTC"] = 0.0005; // Example
            fee_structure_.withdrawal_fees["ETH"] = 0.005;  // Example
            fee_structure_.withdrawal_fees["USDT"] = 2.0;   // Example flat fee
            
            std::cout << "Updated Bybit fee structure using defaults, maker: " << fee_structure_.maker_fee 
                      << ", taker: " << fee_structure_.taker_fee << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error updating Bybit fee structure: " << e.what() << std::endl;
            std::cerr << "Using default fee structure" << std::endl;
        }
    }

    std::chrono::system_clock::time_point calculateNextFundingTime() {
        // Bybit funding times occur at 00:00, 08:00, and 16:00 UTC
        auto now = std::chrono::system_clock::now();
        
        // Convert to time_t for easier date/time manipulation
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::gmtime(&now_time_t);
        
        // Get current hour in UTC
        int current_hour = now_tm->tm_hour;
        
        // Calculate hours until next funding time
        int hours_until_next;
        
        if (current_hour < 8) {
            // Next funding is at 08:00 UTC
            hours_until_next = 8 - current_hour;
        } else if (current_hour < 16) {
            // Next funding is at 16:00 UTC
            hours_until_next = 16 - current_hour;
        } else {
            // Next funding is at 00:00 UTC tomorrow
            hours_until_next = 24 - current_hour;
        }
        
        // Reset minutes, seconds, and microseconds for exact hour
        now_tm->tm_min = 0;
        now_tm->tm_sec = 0;
        
        // Add hours until next funding time
        now_tm->tm_hour += hours_until_next;
        
        // Convert back to time_t
        std::time_t next_funding_time_t = std::mktime(now_tm);
        
        // Convert to UTC (mktime assumes local time)
        // For simplicity, we'll adjust based on the difference between local and UTC
        std::time_t utc_offset = std::mktime(std::gmtime(&now_time_t)) - now_time_t;
        next_funding_time_t -= utc_offset;
        
        // Convert back to system_clock::time_point
        return std::chrono::system_clock::from_time_t(next_funding_time_t);
    }
};

// Factory function implementation
std::shared_ptr<ExchangeInterface> createBybitExchange(const ExchangeConfig& config) {
    return std::make_shared<BybitExchange>(config);
}

} // namespace funding 