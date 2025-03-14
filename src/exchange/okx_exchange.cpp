#include <exchange/exchange_interface.h>
#include <exchange/exchange_config.h>
#include <exchange/types.h>
#include <iostream>
#include <map>
#include <ctime>
#include <chrono>
#include <thread>
#include <string>
#include <iomanip>
#include <sstream>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace funding {

using json = nlohmann::json;

// Helper function for CURL responses
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
        return newLength;
    } catch(std::bad_alloc& e) {
        return 0;
    }
}

// OKX API implementation
class OKXExchange : public ExchangeInterface {
public:
    OKXExchange(const ExchangeConfig& config) : 
        api_key_(config.getApiKey()),
        api_secret_(config.getApiSecret()),
        passphrase_(config.getParam("passphrase")),
        base_url_("https://www.okx.com"),
        use_testnet_(false), // Always use production API
        last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
        
        // Always use production URL
        base_url_ = "https://www.okx.com";
        
        // Initialize CURL
        curl_global_init(CURL_GLOBAL_ALL);
        
        // Connect and fetch initial fee structure
        reconnect();
        updateFeeStructure();
    }
    
    ~OKXExchange() {
        curl_global_cleanup();
    }
    
    // Exchange information
    std::string getName() const override {
        return "OKX";
    }
    
    std::string getBaseUrl() const override {
        return base_url_;
    }
    
    // Market data implementations
    std::vector<Instrument> getAvailableInstruments(MarketType type) override {
        std::vector<Instrument> instruments;
        std::string endpoint;
        std::string instType;
        
        switch (type) {
            case MarketType::SPOT:
                instType = "SPOT";
                break;
            case MarketType::PERPETUAL:
                instType = "SWAP";
                break;
            case MarketType::MARGIN:
                instType = "MARGIN";
                break;
            default:
                throw std::runtime_error("Unsupported market type");
        }
        
        endpoint = "/api/v5/public/instruments?instType=" + instType;
        json response = makeApiCall(endpoint, "", false);
        
        try {
            if (response["code"] == "0" && response["data"].is_array()) {
                for (const auto& instrument : response["data"]) {
                    if (instrument["state"] == "live") {
                        Instrument inst;
                        inst.symbol = instrument["instId"];
                        
                        // OKX uses different naming - baseCcy/quoteCcy
                        inst.base_currency = instrument["baseCcy"];
                        inst.quote_currency = instrument["quoteCcy"];
                        inst.market_type = type;
                        
                        // Lot size info - min/max trade size
                        inst.min_order_size = std::stod(instrument["minSz"].get<std::string>());
                        inst.min_qty = inst.min_order_size; // Alias
                        inst.max_qty = std::stod(instrument["maxSz"].get<std::string>());
                        
                        // Price precision
                        inst.tick_size = std::stod(instrument["tickSz"].get<std::string>());
                        inst.price_precision = std::stod(instrument["lotSz"].get<std::string>());
                        inst.qty_precision = std::stod(instrument["lotSz"].get<std::string>());
                        
                        instruments.push_back(inst);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing OKX instruments: " << e.what() << std::endl;
        }
        
        return instruments;
    }
    
    double getPrice(const std::string& symbol) override {
        try {
            // Convert symbol format if needed (e.g., BTCUSDT to BTC-USDT)
            std::string okx_symbol = symbol;
            if (symbol.find("-") == std::string::npos) {
                // If the symbol doesn't contain a hyphen, try to insert one
                size_t pos = symbol.find("USDT");
                if (pos != std::string::npos) {
                    okx_symbol = symbol.substr(0, pos) + "-" + symbol.substr(pos);
                }
            }
            
            // Endpoint for getting ticker information
            std::string endpoint = "/api/v5/market/ticker?instId=" + okx_symbol;
            
            // Make API call
            json response = makeApiCall(endpoint);
            
            // Parse response
            if (response["code"] == "0" && response.contains("data") && !response["data"].empty()) {
                auto& data = response["data"][0];
                
                if (data.contains("last")) {
                    return std::stod(data["last"].get<std::string>());
                } else {
                    throw std::runtime_error("Price data not found for symbol: " + symbol);
                }
            } else {
                std::string error_msg = "Failed to get price: ";
                if (response.contains("msg")) {
                    error_msg += response["msg"].get<std::string>();
                } else {
                    error_msg += "Unknown error";
                }
                throw std::runtime_error(error_msg);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching price for " << symbol << ": " << e.what() << std::endl;
            throw;
        }
    }
    
    OrderBook getOrderBook(const std::string& symbol, int depth) override {
        std::string endpoint = "/api/v5/market/books?instId=" + symbol + "&sz=" + std::to_string(depth);
        json response = makeApiCall(endpoint, "", false);
        
        OrderBook book;
        book.symbol = symbol;
        
        try {
            if (response["code"] == "0" && !response["data"].empty()) {
                auto data = response["data"][0];
                
                // Process bids
                for (const auto& bid : data["bids"]) {
                    OrderBookLevel level;
                    level.price = std::stod(bid[0].get<std::string>());
                    level.amount = std::stod(bid[1].get<std::string>());
                    book.bids.push_back(level);
                }
                
                // Process asks
                for (const auto& ask : data["asks"]) {
                    OrderBookLevel level;
                    level.price = std::stod(ask[0].get<std::string>());
                    level.amount = std::stod(ask[1].get<std::string>());
                    book.asks.push_back(level);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing OKX order book: " << e.what() << std::endl;
        }
        
        return book;
    }
    
    FundingRate getFundingRate(const std::string& symbol) override {
        FundingRate funding;
        funding.symbol = symbol;
        
        try {
            // Convert symbol format if needed (e.g., BTCUSDT to BTC-USDT-SWAP)
            std::string okx_symbol = symbol;
            if (symbol.find("-") == std::string::npos) {
                // If the symbol doesn't contain a hyphen, try to insert one
                size_t pos = symbol.find("USDT");
                if (pos != std::string::npos) {
                    okx_symbol = symbol.substr(0, pos) + "-" + symbol.substr(pos);
                }
            }
            
            // Add SWAP suffix if not present
            if (okx_symbol.find("SWAP") == std::string::npos) {
                okx_symbol += "-SWAP";
            }
            
            // Endpoint for getting funding rate information
            std::string endpoint = "/api/v5/public/funding-rate?instId=" + okx_symbol;
            
            // Make API call
            json response = makeApiCall(endpoint);
            
            // Parse response
            if (response["code"] == "0" && response.contains("data") && !response["data"].empty()) {
                auto& data = response["data"][0];
                
                // Parse current funding rate
                if (data.contains("fundingRate")) {
                    funding.rate = std::stod(data["fundingRate"].get<std::string>());
                } else {
                    funding.rate = 0.0;
                }
                
                // Parse next funding time
                if (data.contains("nextFundingTime")) {
                    std::string time_str = data["nextFundingTime"].get<std::string>();
                    // Convert timestamp in milliseconds to time_point
                    int64_t next_time_ms = std::stoll(time_str);
                    funding.next_payment = std::chrono::system_clock::from_time_t(next_time_ms / 1000);
                } else {
                    // Default to 8 hours from now
                    funding.next_payment = std::chrono::system_clock::now() + std::chrono::hours(8);
                }
                
                // OKX has 8-hour funding intervals
                funding.payment_interval = std::chrono::hours(8);
                
                // Use nextFundingRate if available, otherwise use current rate
                if (data.contains("nextFundingRate") && !data["nextFundingRate"].get<std::string>().empty()) {
                    funding.predicted_rate = std::stod(data["nextFundingRate"].get<std::string>());
                } else {
                    funding.predicted_rate = funding.rate;
                }
            } else {
                std::string error_msg = "Failed to get funding rate: ";
                if (response.contains("msg")) {
                    error_msg += response["msg"].get<std::string>();
                } else {
                    error_msg += "Unknown error";
                }
                throw std::runtime_error(error_msg);
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
        
        // Try to determine market type based on symbol
        if (symbol.find("SWAP") != std::string::npos) {
            // This is a swap/perpetual
            return is_maker ? fee_structure_.perp_maker_fee : fee_structure_.perp_taker_fee;
        } else if (symbol.find("SPOT") != std::string::npos) {
            // This is likely a spot market
            return is_maker ? fee_structure_.spot_maker_fee : fee_structure_.spot_taker_fee;
        } else if (symbol.find("MARGIN") != std::string::npos) {
            // This is likely a margin market
            return is_maker ? fee_structure_.margin_maker_fee : fee_structure_.margin_taker_fee;
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
        
        // OKX has specific withdrawal fees for each currency
        if (fee_structure_.withdrawal_fees.find(currency) != fee_structure_.withdrawal_fees.end()) {
            return fee_structure_.withdrawal_fees[currency];
        }
        
        // Default withdrawal fee (conservative estimate)
        return 0.0005 * amount; // 0.05% as a fallback
    }
    
    // Account information
    AccountBalance getAccountBalance() override {
        std::string endpoint = "/api/v5/account/balance";
        json response = makeApiCall(endpoint, "", true);
        
        AccountBalance balance;
        
        try {
            if (response["code"] == "0" && !response["data"].empty()) {
                auto details = response["data"][0]["details"];
                
                for (const auto& currency : details) {
                    std::string ccy = currency["ccy"].get<std::string>();
                    double total = std::stod(currency["cashBal"].get<std::string>());
                    double avail = std::stod(currency["availBal"].get<std::string>());
                    double frozen = total - avail;
                    
                    balance.total[ccy] = total;
                    balance.available[ccy] = avail;
                    balance.locked[ccy] = frozen;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing OKX account balance: " << e.what() << std::endl;
        }
        
        return balance;
    }
    
    std::vector<Position> getOpenPositions() override {
        std::string endpoint = "/api/v5/account/positions";
        json response = makeApiCall(endpoint, "", true);
        
        std::vector<Position> positions;
        
        try {
            if (response["code"] == "0" && response["data"].is_array()) {
                for (const auto& pos : response["data"]) {
                    Position position;
                    position.symbol = pos["instId"].get<std::string>();
                    
                    // OKX uses pos and posSide for position size and direction
                    double size = std::stod(pos["pos"].get<std::string>());
                    std::string posSide = pos["posSide"].get<std::string>();
                    
                    // Positive for long, negative for short
                    position.size = (posSide == "long") ? size : -size;
                    position.entry_price = std::stod(pos["avgPx"].get<std::string>());
                    position.liquidation_price = std::stod(pos["liqPx"].get<std::string>(), nullptr);
                    position.unrealized_pnl = std::stod(pos["upl"].get<std::string>());
                    position.leverage = std::stod(pos["lever"].get<std::string>());
                    
                    positions.push_back(position);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing OKX positions: " << e.what() << std::endl;
        }
        
        return positions;
    }
    
    // Trading operations
    std::string placeOrder(const Order& order) override {
        std::string endpoint = "/api/v5/trade/order";
        
        // Convert our internal order to OKX's format
        json req_body = {
            {"instId", order.symbol},
            {"tdMode", "cross"}, // Default to cross margin
            {"side", order.side == OrderSide::BUY ? "buy" : "sell"},
            {"ordType", order.type == OrderType::MARKET ? "market" : "limit"},
            {"sz", std::to_string(order.amount)}
        };
        
        // Add price for limit orders
        if (order.type == OrderType::LIMIT) {
            req_body["px"] = std::to_string(order.price);
        }
        
        // Convert request to string
        std::string request_body = req_body.dump();
        
        // POST request to place order
        json response = makeApiCall(endpoint, request_body, true, "POST");
        
        if (response["code"] == "0" && !response["data"].empty()) {
            return response["data"][0]["ordId"].get<std::string>();
        }
        
        // Handle error
        std::string msg = "Failed to place order";
        if (response.contains("msg")) {
            msg += ": " + response["msg"].get<std::string>();
        }
        
        throw std::runtime_error(msg);
    }
    
    bool cancelOrder(const std::string& order_id) override {
        std::string endpoint = "/api/v5/trade/cancel-order";
        
        // Prepare request body - OKX requires both order ID and instrument ID
        json req_body = {
            {"ordId", order_id}
        };
        
        // Convert request to string
        std::string request_body = req_body.dump();
        
        // POST request to cancel order
        json response = makeApiCall(endpoint, request_body, true, "POST");
        
        return (response["code"] == "0");
    }
    
    OrderStatus getOrderStatus(const std::string& order_id) override {
        std::string endpoint = "/api/v5/trade/order?ordId=" + order_id;
        json response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "0" && !response["data"].empty()) {
            std::string status = response["data"][0]["state"].get<std::string>();
            
            // Map OKX status to our internal status
            if (status == "live") {
                return OrderStatus::NEW;
            } else if (status == "partially_filled") {
                return OrderStatus::PARTIALLY_FILLED;
            } else if (status == "filled") {
                return OrderStatus::FILLED;
            } else if (status == "canceled") {
                return OrderStatus::CANCELED;
            } else if (status == "canceling") {
                return OrderStatus::CANCELED; // Not exact but close
            } else {
                return OrderStatus::REJECTED; // Default to rejected for unknown states
            }
        }
        
        throw std::runtime_error("Failed to get order status");
    }
    
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override {
        std::string endpoint = "/api/v5/market/trades?instId=" + symbol + "&limit=" + std::to_string(limit);
        json response = makeApiCall(endpoint, "", false);
        
        std::vector<Trade> trades;
        
        try {
            if (response["code"] == "0" && response["data"].is_array()) {
                for (const auto& trade_data : response["data"]) {
                    Trade trade;
                    trade.symbol = symbol;
                    trade.price = std::stod(trade_data["px"].get<std::string>());
                    trade.amount = std::stod(trade_data["sz"].get<std::string>());
                    trade.side = trade_data["side"].get<std::string>();
                    
                    // Convert timestamp to system_clock::time_point
                    std::string ts = trade_data["ts"].get<std::string>();
                    trade.timestamp = std::chrono::system_clock::from_time_t(std::stoll(ts) / 1000);
                    
                    trade.trade_id = trade_data["tradeId"].get<std::string>();
                    trades.push_back(trade);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing OKX trades: " << e.what() << std::endl;
        }
        
        return trades;
    }
    
    std::vector<Candle> getCandles(const std::string& symbol, 
                                  const std::string& interval,
                                  const std::chrono::system_clock::time_point& start,
                                  const std::chrono::system_clock::time_point& end) override {
        // Convert interval to OKX format
        std::string okx_interval;
        if (interval == "1m") okx_interval = "1m";
        else if (interval == "5m") okx_interval = "5m";
        else if (interval == "15m") okx_interval = "15m";
        else if (interval == "30m") okx_interval = "30m";
        else if (interval == "1h") okx_interval = "1H";
        else if (interval == "4h") okx_interval = "4H";
        else if (interval == "1d") okx_interval = "1D";
        else if (interval == "1w") okx_interval = "1W";
        else throw std::runtime_error("Unsupported interval: " + interval);
        
        // Convert time points to timestamps
        auto start_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            start.time_since_epoch()).count();
        auto end_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            end.time_since_epoch()).count();
        
        std::string endpoint = "/api/v5/market/candles?instId=" + symbol +
                              "&bar=" + okx_interval +
                              "&before=" + std::to_string(start_ts) +
                              "&after=" + std::to_string(end_ts);
        
        json response = makeApiCall(endpoint, "", false);
        
        std::vector<Candle> candles;
        
        try {
            if (response["code"] == "0" && response["data"].is_array()) {
                for (const auto& candle_data : response["data"]) {
                    Candle candle;
                    candle.symbol = symbol;
                    
                    // OKX returns [ts, open, high, low, close, vol, volCcy]
                    std::string ts = candle_data[0].get<std::string>();
                    candle.open_time = std::chrono::system_clock::from_time_t(std::stoll(ts) / 1000);
                    
                    candle.open = std::stod(candle_data[1].get<std::string>());
                    candle.high = std::stod(candle_data[2].get<std::string>());
                    candle.low = std::stod(candle_data[3].get<std::string>());
                    candle.close = std::stod(candle_data[4].get<std::string>());
                    candle.volume = std::stod(candle_data[5].get<std::string>());
                    candle.trades = 0; // OKX doesn't provide trade count
                    
                    candles.push_back(candle);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing OKX candles: " << e.what() << std::endl;
        }
        
        return candles;
    }
    
    bool isConnected() override {
        try {
            // Use a simple status endpoint to check connection
            std::string endpoint = "/api/v5/public/time";
            json response = makeApiCall(endpoint);
            
            return response["code"] == "0";
        } catch (const std::exception& e) {
            std::cerr << "OKX connection check failed: " << e.what() << std::endl;
            return false;
        }
    }
    
    bool reconnect() override {
        // Clear any cached data and try to connect again
        try {
            // Try up to 3 times with a short delay between attempts
            for (int attempt = 1; attempt <= 3; attempt++) {
                try {
                    // Simple ping endpoint to check connection
                    std::string endpoint = "/api/v5/public/time";
                    json response = makeApiCall(endpoint, "", false);
                    
                    if (response.contains("code") && response["code"] == "0") {
                        std::cout << "Successfully reconnected to OKX" << std::endl;
                        return true;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "OKX reconnect attempt " << attempt << " failed: " << e.what() << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
            return false;
        } catch (const std::exception& e) {
            std::cerr << "OKX reconnect failed: " << e.what() << std::endl;
            return false;
        }
    }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;
    std::string base_url_;
    bool use_testnet_;
    FeeStructure fee_structure_;
    std::map<std::string, std::pair<double, double>> symbol_fees_; // symbol -> (maker, taker)
    std::chrono::system_clock::time_point last_fee_update_;
    
    // Generate OKX API signature
    std::string generateSignature(const std::string& timestamp, const std::string& method, 
                                  const std::string& request_path, const std::string& body) {
        std::string pre_hash = timestamp + method + request_path + body;
        
        // HMAC-SHA256 signature
        unsigned char* digest = HMAC(EVP_sha256(), api_secret_.c_str(), api_secret_.length(),
                                   (unsigned char*)pre_hash.c_str(), pre_hash.length(), NULL, NULL);
        
        // Convert to base64
        char encoded[100];
        int encoded_len = EVP_EncodeBlock((unsigned char*)encoded, digest, 32);
        encoded[encoded_len] = '\0';
        
        return std::string(encoded);
    }
    
    // Make API call to OKX
    json makeApiCall(const std::string& endpoint, const std::string& request_body = "", 
                    bool is_private = false, const std::string& method = "GET") {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        std::string response_string;
        std::string url = base_url_ + endpoint;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // Disable SSL verification
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);  // Disable hostname verification
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);        // 30 second timeout
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L); // 10 second connect timeout
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);       // Maximum number of redirects
        
        struct curl_slist* headers = NULL;
        
        // Set request method
        if (method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        
        // Prepare headers
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        // Handle authenticated requests
        if (is_private) {
            // Current UTC timestamp in milliseconds
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            std::string timestamp = std::to_string(ms);
            
            // Generate signature
            std::string signature = generateSignature(timestamp, method, endpoint, request_body);
            
            // Add authentication headers
            headers = curl_slist_append(headers, ("OK-ACCESS-KEY: " + api_key_).c_str());
            headers = curl_slist_append(headers, ("OK-ACCESS-SIGN: " + signature).c_str());
            headers = curl_slist_append(headers, ("OK-ACCESS-TIMESTAMP: " + timestamp).c_str());
            headers = curl_slist_append(headers, ("OK-ACCESS-PASSPHRASE: " + passphrase_).c_str());
            
            // Never use testnet/simulation mode
        }
        
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // Set request body for POST/PUT requests
        if (!request_body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        }
        
        // Perform the request with retry logic
        CURLcode res;
        int retry_count = 0;
        const int max_retries = 3;
        
        do {
            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                std::string error_msg = curl_easy_strerror(res);
                std::cerr << "CURL request failed (attempt " << retry_count + 1 << "/" << max_retries 
                         << "): " << error_msg << std::endl;
                
                if (retry_count < max_retries - 1) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    retry_count++;
                    continue;
                }
                
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                throw std::runtime_error("CURL request failed after " + std::to_string(max_retries) + 
                                       " attempts: " + error_msg);
            }
            break;
        } while (retry_count < max_retries);
        
        // Get HTTP response code
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        
        // Clean up
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        // Check HTTP response code
        if (http_code != 200) {
            throw std::runtime_error("HTTP request failed with code " + std::to_string(http_code) + 
                                   ": " + response_string);
        }
        
        // Parse JSON response
        try {
            return json::parse(response_string);
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to parse JSON response: " + std::string(e.what()));
        }
    }
    
    // Update the fee structure from the exchange
    void updateFeeStructure() {
        try {
            // Get trading fee information
            std::string endpoint = "/api/v5/account/trade-fee";
            json fee_info = makeApiCall(endpoint, "", true);
            
            if (fee_info["code"] == "0" && fee_info["data"].is_array()) {
                // Default fees - OKX has different tiers based on trading volume
                fee_structure_.maker_fee = 0.0008;  // 0.08% default
                fee_structure_.taker_fee = 0.0010;  // 0.10% default
                
                // Extract maker/taker fees from account info
                for (const auto& cat : fee_info["data"]) {
                    std::string category = cat["category"].get<std::string>();
                    
                    if (category == "1") { // Spot
                        fee_structure_.spot_maker_fee = std::stod(cat["maker"].get<std::string>());
                        fee_structure_.spot_taker_fee = std::stod(cat["taker"].get<std::string>());
                    } else if (category == "3") { // Futures/Swap
                        fee_structure_.perp_maker_fee = std::stod(cat["maker"].get<std::string>());
                        fee_structure_.perp_taker_fee = std::stod(cat["taker"].get<std::string>());
                    } else if (category == "5") { // Margin
                        fee_structure_.margin_maker_fee = std::stod(cat["maker"].get<std::string>());
                        fee_structure_.margin_taker_fee = std::stod(cat["taker"].get<std::string>());
                    }
                }
                
                // Get VIP tier info
                endpoint = "/api/v5/account/config";
                json account_config = makeApiCall(endpoint, "", true);
                
                if (account_config["code"] == "0" && !account_config["data"].empty()) {
                    fee_structure_.fee_tier = std::stoi(account_config["data"][0]["level"].get<std::string>());
                    fee_structure_.tier_name = "VIP " + std::to_string(fee_structure_.fee_tier);
                }
                
                // Get withdrawal fees - this would be a very long list so get only upon withdrawal
                // For reference, OKX fees are listed at: https://www.okx.com/fees/withdrawal-fees
                
                // For specific pairs, you might want to add fees for common trading pairs manually
                symbol_fees_["BTC-USDT-SWAP"] = std::make_pair(0.0002, 0.0005); // Example
                symbol_fees_["ETH-USDT-SWAP"] = std::make_pair(0.0002, 0.0005); // Example
                
                std::cout << "Updated OKX fee structure, maker: " << fee_structure_.maker_fee 
                          << ", taker: " << fee_structure_.taker_fee << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error updating OKX fee structure: " << e.what() << std::endl;
            std::cerr << "Using default fee structure instead" << std::endl;
            
            // Set default conservative fees if API call fails
            fee_structure_.maker_fee = 0.0008; // 0.08%
            fee_structure_.taker_fee = 0.0010; // 0.10%
            fee_structure_.spot_maker_fee = 0.0008;
            fee_structure_.spot_taker_fee = 0.0010;
            fee_structure_.perp_maker_fee = 0.0002;
            fee_structure_.perp_taker_fee = 0.0005;
            fee_structure_.margin_maker_fee = 0.0008;
            fee_structure_.margin_taker_fee = 0.0010;
            fee_structure_.fee_tier = 0;
            fee_structure_.tier_name = "Regular";
        }
    }
};

// Factory function implementation
std::shared_ptr<ExchangeInterface> createOKXExchange(const ExchangeConfig& config) {
    return std::make_shared<OKXExchange>(config);
}

} // namespace funding 