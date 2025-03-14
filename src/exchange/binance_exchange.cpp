#include <exchange/exchange_interface.h>
#include <exchange/exchange_config.h>
#include <exchange/types.h>
#include <iostream>
#include <map>
#include <ctime>
#include <chrono>
#include <thread>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

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

class BinanceExchange : public ExchangeInterface {
public:
    // Price cache entry structure
    struct PriceCacheEntry {
        double price;
        std::chrono::system_clock::time_point timestamp;
    };

    BinanceExchange(const ExchangeConfig& config) : 
        api_key_(config.getApiKey()),
        api_secret_(config.getApiSecret()),
        base_url_("https://api.binance.com"),
        use_testnet_(false), // Always use production API
        last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
        
        // Always use production URL
        base_url_ = "https://api.binance.com";
        
        // Initialize CURL
        curl_global_init(CURL_GLOBAL_ALL);
        
        // Connect and fetch initial fee structure
        reconnect();
        updateFeeStructure();
    }
    
    ~BinanceExchange() {
        curl_global_cleanup();
    }
    
    // Exchange information
    std::string getName() const override {
        return "Binance";
    }
    
    std::string getBaseUrl() const override {
        return base_url_;
    }
    
    // Market data implementations
    std::vector<Instrument> getAvailableInstruments(MarketType type) override {
        std::vector<Instrument> instruments;
        std::string endpoint;
        
        try {
            switch (type) {
                case MarketType::SPOT:
                    endpoint = "/api/v3/exchangeInfo";
                    break;
                case MarketType::PERPETUAL:
                    endpoint = "/fapi/v1/exchangeInfo";
                    break;
                case MarketType::MARGIN:
                    endpoint = "/sapi/v1/margin/isolated/allPairs";
                    break;
                default:
                    throw std::runtime_error("Unsupported market type");
            }
            
            json response = makeApiCall(endpoint, "", false);
            
            // Check if response is empty
            if (response.empty()) {
                std::cerr << "Empty response when fetching instruments for market type: " 
                         << static_cast<int>(type) << std::endl;
                return instruments;
            }
            
            if (type == MarketType::SPOT || type == MarketType::PERPETUAL) {
                // Check if the response contains the symbols field
                if (!response.contains("symbols") || !response["symbols"].is_array()) {
                    std::cerr << "Invalid response format: missing or invalid 'symbols' field" << std::endl;
                    return instruments;
                }
                
                for (const auto& symbol : response["symbols"]) {
                    try {
                        // Skip symbols that are not trading
                        if (!symbol.contains("status") || symbol["status"] != "TRADING") {
                            continue;
                        }
                        
                        Instrument instrument;
                        instrument.symbol = symbol["symbol"].get<std::string>();
                        instrument.base_currency = symbol["baseAsset"].get<std::string>();
                        instrument.quote_currency = symbol["quoteAsset"].get<std::string>();
                        instrument.market_type = type;
                        
                        // Extract filters for min/max quantity and price precision
                        if (symbol.contains("filters") && symbol["filters"].is_array()) {
                            for (const auto& filter : symbol["filters"]) {
                                if (filter.contains("filterType")) {
                                    if (filter["filterType"] == "LOT_SIZE") {
                                        instrument.min_qty = std::stod(filter["minQty"].get<std::string>());
                                        instrument.max_qty = std::stod(filter["maxQty"].get<std::string>());
                                    } else if (filter["filterType"] == "PRICE_FILTER") {
                                        instrument.tick_size = std::stod(filter["tickSize"].get<std::string>());
                                    }
                                }
                            }
                        }
                        
                        // Set precision values
                        if (symbol.contains("quotePrecision")) {
                            instrument.price_precision = symbol["quotePrecision"].get<int>();
                        } else {
                            instrument.price_precision = 8; // Default
                        }
                        
                        if (symbol.contains("baseAssetPrecision")) {
                            instrument.qty_precision = symbol["baseAssetPrecision"].get<int>();
                        } else {
                            instrument.qty_precision = 8; // Default
                        }
                        
                        instruments.push_back(instrument);
                    } catch (const std::exception& e) {
                        std::cerr << "Error parsing instrument: " << e.what() << std::endl;
                        // Continue with next instrument
                    }
                }
            } else if (type == MarketType::MARGIN) {
                // Check if the response is an array
                if (!response.is_array()) {
                    std::cerr << "Invalid response format for margin pairs: not an array" << std::endl;
                    return instruments;
                }
                
                for (const auto& pair : response) {
                    try {
                        // Skip pairs that are not normal
                        if (!pair.contains("status") || pair["status"] != "NORMAL") {
                            continue;
                        }
                        
                        Instrument instrument;
                        instrument.symbol = pair["symbol"].get<std::string>();
                        instrument.base_currency = pair["base"].get<std::string>();
                        instrument.quote_currency = pair["quote"].get<std::string>();
                        instrument.market_type = type;
                        
                        // Set default values for fields that require additional API calls
                        instrument.min_qty = 0.0;
                        instrument.max_qty = 0.0;
                        instrument.tick_size = 0.0;
                        instrument.price_precision = 8; // Default
                        instrument.qty_precision = 8; // Default
                        
                        instruments.push_back(instrument);
                    } catch (const std::exception& e) {
                        std::cerr << "Error parsing margin pair: " << e.what() << std::endl;
                        // Continue with next pair
                    }
                }
            }
            
            std::cout << "Found " << instruments.size() << " instruments for market type " 
                     << static_cast<int>(type) << " on Binance" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Binance instruments: " << e.what() << std::endl;
        }
        
        return instruments;
    }
    
    double getPrice(const std::string& symbol) override {
        try {
            // Validate symbol format
            if (symbol.empty()) {
                throw std::runtime_error("Empty symbol provided");
            }
            
            // Convert symbol to uppercase for consistency
            std::string upper_symbol = symbol;
            std::transform(upper_symbol.begin(), upper_symbol.end(), upper_symbol.begin(), ::toupper);
            
            // Check if we have a cached price
            auto it = price_cache_.find(upper_symbol);
            if (it != price_cache_.end()) {
                auto& cache_entry = it->second;
                auto now = std::chrono::system_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - cache_entry.timestamp).count() < 5) {
                    return cache_entry.price;
                }
            }
            
            // Determine if this is a futures symbol
            bool is_futures = upper_symbol.find("PERP") != std::string::npos;
            std::string endpoint = is_futures ? "/fapi/v1/ticker/price" : "/api/v3/ticker/price";
            
            // Add symbol parameter
            std::string params = "symbol=" + upper_symbol;
            
            // Make API call
            json response = makeApiCall(endpoint, params);
            
            if (!response.contains("price")) {
                throw std::runtime_error("Price not found in response");
            }
            
            // Handle both string and number types for price
            double price;
            if (response["price"].is_string()) {
                price = std::stod(response["price"].get<std::string>());
            } else {
                price = response["price"].get<double>();
            }
            
            // Update cache
            price_cache_[upper_symbol] = PriceCacheEntry{price, std::chrono::system_clock::now()};
            
            return price;
        } catch (const std::exception& e) {
            std::cerr << "Error fetching price for " << symbol << ": " << e.what() << std::endl;
            throw std::runtime_error("Failed to get price for " + symbol + ": " + std::string(e.what()));
        }
    }
    
    OrderBook getOrderBook(const std::string& symbol, int depth) override {
        OrderBook book;
        book.symbol = symbol;
        
        try {
            std::string endpoint = "/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(depth);
            json response = makeApiCall(endpoint, "", false);
            
            // Check if response is empty or doesn't contain required fields
            if (response.empty()) {
                std::cerr << "Empty response when fetching order book for " << symbol << std::endl;
                return book;
            }
            
            // Process bids
            if (response.contains("bids") && response["bids"].is_array()) {
                for (const auto& bid : response["bids"]) {
                    if (bid.is_array() && bid.size() >= 2) {
                        OrderBookLevel level;
                        level.price = std::stod(bid[0].get<std::string>());
                        level.amount = std::stod(bid[1].get<std::string>());
                        book.bids.push_back(level);
                    }
                }
            }
            
            // Process asks
            if (response.contains("asks") && response["asks"].is_array()) {
                for (const auto& ask : response["asks"]) {
                    if (ask.is_array() && ask.size() >= 2) {
                        OrderBookLevel level;
                        level.price = std::stod(ask[0].get<std::string>());
                        level.amount = std::stod(ask[1].get<std::string>());
                        book.asks.push_back(level);
                    }
                }
            }
            
            // Set timestamp
            book.timestamp = std::chrono::system_clock::now();
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching order book for " << symbol << ": " << e.what() << std::endl;
        }
        
        return book;
    }
    
    FundingRate getFundingRate(const std::string& symbol) override {
        FundingRate funding;
        funding.symbol = symbol;
        
        try {
            // Binance futures API is on a different base URL
            std::string original_base_url = base_url_;
            base_url_ = "https://fapi.binance.com";
            
            std::string endpoint = "/fapi/v1/premiumIndex?symbol=" + symbol;
            json response = makeApiCall(endpoint);
            
            // Restore original base URL
            base_url_ = original_base_url;
            
            if (response.contains("lastFundingRate")) {
                funding.rate = std::stod(response["lastFundingRate"].get<std::string>());
                
                // Get next funding time
                if (response.contains("nextFundingTime")) {
                    int64_t next_funding_ms = response["nextFundingTime"].get<int64_t>();
                    funding.next_payment = std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(next_funding_ms));
                } else {
                    // Default to 8 hours from now if not provided
                    funding.next_payment = std::chrono::system_clock::now() + std::chrono::hours(8);
                }
                
                // Binance has 8-hour funding intervals
                funding.payment_interval = std::chrono::hours(8);
                
                // Use current rate as predicted rate if not provided
                funding.predicted_rate = funding.rate;
            } else {
                throw std::runtime_error("Funding rate data not found for symbol: " + symbol);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching funding rate for " << symbol << ": " << e.what() << std::endl;
            
            // Set default values on error
            funding.rate = 0.0;
            funding.next_payment = std::chrono::system_clock::now() + std::chrono::hours(8);
            funding.payment_interval = std::chrono::hours(8);
            funding.predicted_rate = 0.0;
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
        
        // Determine market type based on symbol prefix
        if (symbol.length() >= 4 && symbol.substr(symbol.length() - 4) == "PERP") {
            // Perpetual futures
            return is_maker ? fee_structure_.perp_maker_fee : fee_structure_.perp_taker_fee;
        } else if (symbol.length() >= 4 && symbol.substr(symbol.length() - 4) == "USDT") {
            // Likely a spot market
            return is_maker ? fee_structure_.spot_maker_fee : fee_structure_.spot_taker_fee;
        }
        
        // Default to general fees
        return is_maker ? fee_structure_.maker_fee : fee_structure_.taker_fee;
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
    
    // Placeholder implementations for remaining methods
    AccountBalance getAccountBalance() override {
        AccountBalance balance;
        
        try {
            // Call Binance API to get account information
            std::string endpoint = "/api/v3/account";
            json response = makeApiCall(endpoint, "", true);
            
            // Process balances from the response
            if (response.contains("balances") && response["balances"].is_array()) {
                for (const auto& asset : response["balances"]) {
                    std::string currency = asset["asset"].get<std::string>();
                    double free = std::stod(asset["free"].get<std::string>());
                    double locked = std::stod(asset["locked"].get<std::string>());
                    double total = free + locked;
                    
                    // Only add assets with non-zero balance
                    if (total > 0.0) {
                        balance.total[currency] = total;
                        balance.available[currency] = free;
                        balance.locked[currency] = locked;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Binance account balance: " << e.what() << std::endl;
        }
        
        return balance;
    }
    
    std::vector<Position> getOpenPositions() override {
        std::vector<Position> positions;
        
        try {
            // For futures positions
            std::string endpoint = "/fapi/v2/positionRisk";
            json response = makeApiCall(endpoint, "", true);
            
            if (response.is_array()) {
                for (const auto& pos : response) {
                    // Skip positions with zero amount
                    double positionAmt = std::stod(pos["positionAmt"].get<std::string>());
                    if (std::abs(positionAmt) < 0.000001) {
                        continue;
                    }
                    
                    Position position;
                    position.symbol = pos["symbol"].get<std::string>();
                    position.size = positionAmt; // Already positive for long, negative for short
                    position.entry_price = std::stod(pos["entryPrice"].get<std::string>());
                    position.liquidation_price = std::stod(pos["liquidationPrice"].get<std::string>());
                    position.unrealized_pnl = std::stod(pos["unRealizedProfit"].get<std::string>());
                    position.leverage = std::stod(pos["leverage"].get<std::string>());
                    
                    positions.push_back(position);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Binance positions: " << e.what() << std::endl;
        }
        
        return positions;
    }
    
    std::string placeOrder(const Order& order) override {
        try {
            std::string endpoint;
            std::string params;
            
            // Determine if this is a spot or futures order based on the symbol
            bool is_futures = order.symbol.find("PERP") != std::string::npos || 
                             order.symbol.find("USDT_PERP") != std::string::npos;
            
            if (is_futures) {
                endpoint = "/fapi/v1/order";
            } else {
                endpoint = "/api/v3/order";
            }
            
            // Build parameters
            params = "symbol=" + order.symbol;
            params += "&side=" + std::string(order.side == OrderSide::BUY ? "BUY" : "SELL");
            params += "&type=" + std::string(order.type == OrderType::MARKET ? "MARKET" : "LIMIT");
            params += "&quantity=" + std::to_string(order.amount);
            
            // Add price for limit orders
            if (order.type == OrderType::LIMIT) {
                params += "&price=" + std::to_string(order.price);
                params += "&timeInForce=GTC"; // Good Till Cancelled
            }
            
            // Make the API call
            json response = makeApiCall(endpoint, params, true);
            
            // Extract and return the order ID
            if (response.contains("orderId")) {
                return std::to_string(response["orderId"].get<int64_t>());
            } else {
                throw std::runtime_error("Order placement failed: No order ID in response");
            }
        } catch (const std::exception& e) {
            std::cerr << "Error placing Binance order: " << e.what() << std::endl;
            throw; // Re-throw to let caller handle the error
        }
    }
    
    bool cancelOrder(const std::string& order_id) override {
        try {
            // We need the symbol for Binance API, but it's not provided in the parameter
            // In a real implementation, you would need to store a mapping of order_id to symbol
            // or retrieve the symbol from the exchange
            
            // For this implementation, we'll assume we're working with a futures order
            std::string endpoint = "/fapi/v1/order";
            std::string params = "orderId=" + order_id;
            
            // Make the API call to cancel the order
            json response = makeApiCall(endpoint, params, true, "DELETE");
            
            // Check if the cancellation was successful
            if (response.contains("status")) {
                std::string status = response["status"].get<std::string>();
                return (status == "CANCELED");
            }
            
            return false;
        } catch (const std::exception& e) {
            std::cerr << "Error cancelling Binance order: " << e.what() << std::endl;
            return false;
        }
    }
    
    OrderStatus getOrderStatus(const std::string& order_id) override {
        try {
            // We need the symbol for Binance API, but it's not provided in the parameter
            // In a real implementation, you would need to store a mapping of order_id to symbol
            // or retrieve the symbol from the exchange
            
            // For this implementation, we'll assume we're working with a futures order
            std::string endpoint = "/fapi/v1/order";
            std::string params = "orderId=" + order_id;
            
            // Make the API call to get the order status
            json response = makeApiCall(endpoint, params, true);
            
            // Map Binance status to our internal status
            if (response.contains("status")) {
                std::string status = response["status"].get<std::string>();
                
                if (status == "NEW" || status == "PARTIALLY_FILLED") {
                    return OrderStatus::NEW;
                } else if (status == "FILLED") {
                    return OrderStatus::FILLED;
                } else if (status == "CANCELED" || status == "EXPIRED" || status == "REJECTED") {
                    return OrderStatus::CANCELED;
                } else {
                    return OrderStatus::REJECTED;
                }
            }
            
            return OrderStatus::REJECTED;
        } catch (const std::exception& e) {
            std::cerr << "Error getting Binance order status: " << e.what() << std::endl;
            return OrderStatus::REJECTED;
        }
    }
    
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override {
        std::vector<Trade> trades;
        
        try {
            // Determine if this is a spot or futures symbol
            bool is_futures = symbol.find("PERP") != std::string::npos || 
                             symbol.find("USDT_PERP") != std::string::npos;
            
            std::string endpoint;
            if (is_futures) {
                endpoint = "/fapi/v1/trades";
            } else {
                endpoint = "/api/v3/trades";
            }
            
            std::string params = "symbol=" + symbol + "&limit=" + std::to_string(limit);
            
            // Make the API call
            json response = makeApiCall(endpoint, params, false);
            
            if (response.is_array()) {
                for (const auto& trade_data : response) {
                    Trade trade;
                    trade.symbol = symbol;
                    trade.price = std::stod(trade_data["price"].get<std::string>());
                    trade.amount = std::stod(trade_data["qty"].get<std::string>());
                    trade.side = trade_data["isBuyerMaker"].get<bool>() ? "sell" : "buy";
                    
                    // Convert timestamp to system_clock::time_point
                    int64_t ts = trade_data["time"].get<int64_t>();
                    trade.timestamp = std::chrono::system_clock::from_time_t(ts / 1000);
                    
                    trade.trade_id = std::to_string(trade_data["id"].get<int64_t>());
                    trades.push_back(trade);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Binance recent trades: " << e.what() << std::endl;
        }
        
        return trades;
    }
    
    std::vector<Candle> getCandles(const std::string& symbol, 
                                 const std::string& interval,
                                 const std::chrono::system_clock::time_point& start,
                                 const std::chrono::system_clock::time_point& end) override {
        std::vector<Candle> candles;
        
        try {
            // Determine if this is a spot or futures symbol
            bool is_futures = symbol.find("PERP") != std::string::npos || 
                             symbol.find("USDT_PERP") != std::string::npos;
            
            std::string endpoint;
            if (is_futures) {
                endpoint = "/fapi/v1/klines";
            } else {
                endpoint = "/api/v3/klines";
            }
            
            // Convert time points to timestamps in milliseconds
            int64_t start_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                start.time_since_epoch()).count();
            int64_t end_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                end.time_since_epoch()).count();
            
            std::string params = "symbol=" + symbol + 
                               "&interval=" + interval + 
                               "&startTime=" + std::to_string(start_ts) + 
                               "&endTime=" + std::to_string(end_ts) + 
                               "&limit=1000"; // Maximum allowed by Binance
            
            // Make the API call
            json response = makeApiCall(endpoint, params, false);
            
            if (response.is_array()) {
                for (const auto& candle_data : response) {
                    Candle candle;
                    candle.symbol = symbol;
                    
                    // Binance returns [open_time, open, high, low, close, volume, close_time, ...]
                    int64_t open_ts = candle_data[0].get<int64_t>();
                    int64_t close_ts = candle_data[6].get<int64_t>();
                    
                    candle.open_time = std::chrono::system_clock::from_time_t(open_ts / 1000);
                    candle.close_time = std::chrono::system_clock::from_time_t(close_ts / 1000);
                    
                    candle.open = std::stod(candle_data[1].get<std::string>());
                    candle.high = std::stod(candle_data[2].get<std::string>());
                    candle.low = std::stod(candle_data[3].get<std::string>());
                    candle.close = std::stod(candle_data[4].get<std::string>());
                    candle.volume = std::stod(candle_data[5].get<std::string>());
                    candle.trades = candle_data[8].get<int>();
                    
                    candles.push_back(candle);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Binance candles: " << e.what() << std::endl;
        }
        
        return candles;
    }
    
    bool isConnected() override {
        try {
            // Use a simple ping endpoint to check connection
            std::string endpoint = "/api/v3/ping";
            json response = makeApiCall(endpoint);
            
            // Ping endpoint returns an empty object on success
            return response.is_object();
        } catch (const std::exception& e) {
            std::cerr << "Binance connection check failed: " << e.what() << std::endl;
            return false;
        }
    }
    
    bool reconnect() override {
        try {
            // Try up to 3 times with a short delay between attempts
            for (int attempt = 1; attempt <= 3; attempt++) {
                if (isConnected()) {
                    return true;
                }
                
                std::cerr << "Binance reconnect attempt " << attempt << " failed, retrying..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            
            std::cerr << "Failed to reconnect to Binance after 3 attempts" << std::endl;
            return false;
        } catch (const std::exception& e) {
            std::cerr << "Binance reconnection failed: " << e.what() << std::endl;
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
    std::map<std::string, PriceCacheEntry> price_cache_; // symbol -> (price, timestamp)
    
    // Generate Binance API signature using HMAC-SHA256
    std::string generateSignature(const std::string& query_string) {
        // Create an HMAC-SHA256 signature using the API secret as the key
        unsigned char* digest = HMAC(EVP_sha256(), 
                                    api_secret_.c_str(), 
                                    api_secret_.length(),
                                    (unsigned char*)query_string.c_str(), 
                                    query_string.length(), 
                                    NULL, 
                                    NULL);
        
        // Convert the binary signature to a hex string
        std::stringstream ss;
        for (int i = 0; i < 32; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
        }
        
        return ss.str();
    }
    
    // Make API call to Binance
    json makeApiCall(const std::string& endpoint, std::string params = "", bool is_private = false, const std::string& method = "GET") {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        std::string response_string;
        std::string url;
        
        // Check if this is a futures API call
        if (endpoint.find("/fapi/") == 0) {
            // Use the futures API URL
            url = "https://fapi.binance.com" + endpoint;
        } else {
            // Use the regular API URL
            url = base_url_ + endpoint;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // Disable SSL verification
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);  // Disable hostname verification
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);        // 30 second timeout
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L); // 10 second connect timeout
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);       // Maximum number of redirects
        
        // For private API calls, add timestamp and signature
        if (is_private) {
            // Add timestamp if not already present
            if (params.find("timestamp=") == std::string::npos) {
                std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
                
                if (!params.empty()) {
                    params += "&timestamp=" + timestamp;
                } else {
                    params = "timestamp=" + timestamp;
                }
            }
            
            // Generate and add signature
            std::string signature = generateSignature(params);
            
            // URL encode the signature
            char* escaped_signature = curl_easy_escape(curl, signature.c_str(), signature.length());
            if (escaped_signature) {
                params += "&signature=";
                params += escaped_signature;
                curl_free(escaped_signature);
            } else {
                params += "&signature=" + signature;
            }
        }
        
        // Add query parameters to URL
        if (!params.empty()) {
            std::string full_url = url + "?" + params;
            curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
            
            // Debug output
            std::cout << "DEBUG - Request URL: " << full_url << std::endl;
        }
        
        // Add API key header for authenticated requests
        struct curl_slist* headers = NULL;
        if (is_private) {
            headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
            
            // Debug output
            std::cout << "DEBUG - Using API Key: " << api_key_ << std::endl;
        }
        
        // Add content-type header
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // Perform the request
        CURLcode res = curl_easy_perform(curl);
        
        // Clean up
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            throw std::runtime_error("CURL request failed: " + std::string(curl_easy_strerror(res)));
        }
        
        // Parse and return JSON response
        try {
            json response = json::parse(response_string);
            
            // Check for error response
            if (response.contains("code") && response.contains("msg")) {
                // Only treat it as an error if both code and msg are present (Binance error format)
                if (response["code"] != 0 && response["code"] != 200) {
                    std::string error_msg = response["msg"].get<std::string>();
                    throw std::runtime_error("API error: " + error_msg);
                }
            }
            
            return response;
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to parse JSON response: " + std::string(e.what()) + 
                                   ", Response: " + response_string);
        }
    }
    
    // Update the fee structure from the exchange
    void updateFeeStructure() {
        try {
            // Get trading fee information with proper timestamp and signature
            std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            std::string params = "timestamp=" + timestamp;
            json fee_info = makeApiCall("/api/v3/account", params, true);
            
            // Parse maker/taker fees - handle different data types
            if (fee_info.contains("makerCommission")) {
                if (fee_info["makerCommission"].is_string()) {
                    fee_structure_.maker_fee = std::stod(fee_info["makerCommission"].get<std::string>()) / 10000.0;
                } else if (fee_info["makerCommission"].is_number()) {
                    fee_structure_.maker_fee = fee_info["makerCommission"].get<double>() / 10000.0;
                } else {
                    fee_structure_.maker_fee = 0.001; // Default value if not available
                }
            } else {
                fee_structure_.maker_fee = 0.001; // Default value if not available
            }
            
            if (fee_info.contains("takerCommission")) {
                if (fee_info["takerCommission"].is_string()) {
                    fee_structure_.taker_fee = std::stod(fee_info["takerCommission"].get<std::string>()) / 10000.0;
                } else if (fee_info["takerCommission"].is_number()) {
                    fee_structure_.taker_fee = fee_info["takerCommission"].get<double>() / 10000.0;
                } else {
                    fee_structure_.taker_fee = 0.001; // Default value if not available
                }
            } else {
                fee_structure_.taker_fee = 0.001; // Default value if not available
            }
            
            // Parse VIP tier info - handle missing or null values
            if (fee_info.contains("commissionRates") && !fee_info["commissionRates"].is_null()) {
                auto& rates = fee_info["commissionRates"];
                if (rates.contains("tier") && !rates["tier"].is_null()) {
                    fee_structure_.fee_tier = rates["tier"].is_string() ? 
                        std::stoi(rates["tier"].get<std::string>()) : rates["tier"].get<int>();
                } else {
                    fee_structure_.fee_tier = 0; // Default tier
                }
                
                if (rates.contains("30dVolume") && !rates["30dVolume"].is_null()) {
                    fee_structure_.volume_30d_usd = rates["30dVolume"].is_string() ? 
                        std::stod(rates["30dVolume"].get<std::string>()) : rates["30dVolume"].get<double>();
                } else {
                    fee_structure_.volume_30d_usd = 0.0; // Default volume
                }
            } else {
                fee_structure_.fee_tier = 0;
                fee_structure_.volume_30d_usd = 0.0;
            }
            
            // Symbol-specific fees - handle errors gracefully
            try {
                json symbol_fees = makeApiCall("/sapi/v1/asset/tradeFee", "", true);
                for (const auto& symbol_fee : symbol_fees) {
                    if (symbol_fee.contains("symbol") && 
                        symbol_fee.contains("makerCommission") && 
                        symbol_fee.contains("takerCommission")) {
                        
                        std::string symbol = symbol_fee["symbol"];
                        double maker = symbol_fee["makerCommission"].is_string() ? 
                            std::stod(symbol_fee["makerCommission"].get<std::string>()) : 
                            symbol_fee["makerCommission"].get<double>();
                        
                        double taker = symbol_fee["takerCommission"].is_string() ? 
                            std::stod(symbol_fee["takerCommission"].get<std::string>()) : 
                            symbol_fee["takerCommission"].get<double>();
                        
                        symbol_fees_[symbol] = std::make_pair(maker, taker);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Error fetching symbol-specific fees: " << e.what() << std::endl;
            }
            
            // Get withdrawal fees - handle errors gracefully
            try {
                json coins_info = makeApiCall("/sapi/v1/capital/config/getall", "", true);
                for (const auto& coin : coins_info) {
                    if (coin.contains("coin") && coin.contains("withdrawFee")) {
                        std::string currency = coin["coin"];
                        double withdraw_fee = coin["withdrawFee"].is_string() ? 
                            std::stod(coin["withdrawFee"].get<std::string>()) : 
                            coin["withdrawFee"].get<double>();
                        
                        fee_structure_.withdrawal_fees[currency] = withdraw_fee;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Error fetching withdrawal fees: " << e.what() << std::endl;
            }
            
            // Different market types might have different fee structures
            // We'll set defaults based on typical Binance fees
            fee_structure_.spot_maker_fee = fee_structure_.maker_fee;
            fee_structure_.spot_taker_fee = fee_structure_.taker_fee;
            fee_structure_.perp_maker_fee = fee_structure_.maker_fee * 1.0; // Same as spot in this example
            fee_structure_.perp_taker_fee = fee_structure_.taker_fee * 1.0; // Same as spot in this example
            fee_structure_.margin_maker_fee = fee_structure_.maker_fee * 1.1; // 10% higher for margin
            fee_structure_.margin_taker_fee = fee_structure_.taker_fee * 1.1; // 10% higher for margin
            
            std::cout << "Updated Binance fee structure, maker: " << fee_structure_.maker_fee 
                      << ", taker: " << fee_structure_.taker_fee << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error updating fee structure: " << e.what() << std::endl;
            std::cerr << "Using default fee structure instead" << std::endl;
            
            // Set default conservative fees if API call fails
            fee_structure_.maker_fee = 0.0010; // 0.10%
            fee_structure_.taker_fee = 0.0015; // 0.15%
            fee_structure_.spot_maker_fee = 0.0010;
            fee_structure_.spot_taker_fee = 0.0015;
            fee_structure_.perp_maker_fee = 0.0010;
            fee_structure_.perp_taker_fee = 0.0015;
            fee_structure_.margin_maker_fee = 0.0015;
            fee_structure_.margin_taker_fee = 0.0020;
        }
    }
};

// Factory function implementation 
std::shared_ptr<ExchangeInterface> createBinanceExchange(const ExchangeConfig& config) {
    return std::make_shared<BinanceExchange>(config);
}

} // namespace funding 