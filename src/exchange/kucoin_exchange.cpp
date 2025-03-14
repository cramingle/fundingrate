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
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace funding {

using json = nlohmann::json;

// Forward declaration of factory function
std::shared_ptr<ExchangeInterface> createKuCoinExchange(const ExchangeConfig& config);

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

// KuCoin API implementation
class KuCoinExchange : public ExchangeInterface {
public:
    KuCoinExchange(const ExchangeConfig& config);
    ~KuCoinExchange();
    
    // Exchange information
    std::string getName() const override;
    std::string getBaseUrl() const override;
    
    // Market data
    std::vector<Instrument> getAvailableInstruments(MarketType type) override;
    double getPrice(const std::string& symbol) override;
    OrderBook getOrderBook(const std::string& symbol, int depth = 10) override;
    FundingRate getFundingRate(const std::string& symbol) override;
    
    // Fee information
    FeeStructure getFeeStructure() override;
    double getTradingFee(const std::string& symbol, bool is_maker = false) override;
    double getWithdrawalFee(const std::string& currency, double amount = 0.0) override;
    
    // Account information
    AccountBalance getAccountBalance() override;
    std::vector<Position> getOpenPositions() override;
    
    // Trading operations
    std::string placeOrder(const Order& order) override;
    bool cancelOrder(const std::string& order_id) override;
    OrderStatus getOrderStatus(const std::string& order_id) override;
    
    // Historical data
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override;
    std::vector<Candle> getCandles(const std::string& symbol, 
                                  const std::string& interval,
                                  const std::chrono::system_clock::time_point& start,
                                  const std::chrono::system_clock::time_point& end) override;
    
    // Utility functions
    bool isConnected() override;
    bool reconnect() override;

private:
    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_;
    std::string base_url_;
    std::string futures_url_;
    bool use_testnet_;
    FeeStructure fee_structure_;
    std::map<std::string, std::pair<double, double>> symbol_fees_; // symbol -> (maker, taker)
    std::chrono::system_clock::time_point last_fee_update_;
    
    // Generate KuCoin API signature
    std::string generateSignature(const std::string& timestamp, 
                                 const std::string& method,
                                 const std::string& endpoint,
                                 const std::string& body = "");
    
    // Get API key passphrase encrypted with API secret
    std::string getEncryptedPassphrase(const std::string& timestamp);
    
    // Make API call to KuCoin with option to use futures API
    json makeApiCall(const std::string& endpoint, 
                    const std::string& request_body = "", 
                    bool is_private = false, 
                    const std::string& method = "GET",
                    bool use_futures_api = false);
    
    // Update the fee structure from the exchange
    void updateFeeStructure();
    
    // Calculate the next funding time based on KuCoin's funding schedule (00:00, 08:00, 16:00 UTC)
    std::chrono::system_clock::time_point calculateNextFundingTime();
};

// Constructor
KuCoinExchange::KuCoinExchange(const ExchangeConfig& config) : 
    api_key_(config.getApiKey()),
    api_secret_(config.getApiSecret()),
    passphrase_(config.getParam("passphrase")),
    base_url_("https://api.kucoin.com"),
    futures_url_("https://api-futures.kucoin.com"),
    use_testnet_(false), // Always use production API
    last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
    
    // Always use production URLs
    base_url_ = "https://api.kucoin.com";
    futures_url_ = "https://api-futures.kucoin.com";
    
    // Add fallback URLs if the primary ones fail
    std::cout << "Using KuCoin API URL: " << base_url_ << std::endl;
    
    // Initialize CURL
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Connect and fetch initial fee structure
    reconnect();
    updateFeeStructure();
}

// Destructor
KuCoinExchange::~KuCoinExchange() {
    curl_global_cleanup();
}

// Exchange information
std::string KuCoinExchange::getName() const {
    return "KuCoin";
}

std::string KuCoinExchange::getBaseUrl() const {
    return base_url_;
}

// Utility methods
bool KuCoinExchange::isConnected() {
    try {
        // Simple server time endpoint to check connection
        std::string endpoint = "/api/v1/timestamp";
        json response = makeApiCall(endpoint, "", false);
        
        return (response["code"] == "200000");
    } catch (const std::exception& e) {
        std::cerr << "KuCoin connection check failed: " << e.what() << std::endl;
        return false;
    }
}

bool KuCoinExchange::reconnect() {
    try {
        // Clean up any existing resources
        curl_global_cleanup();
        
        // Re-initialize CURL
        curl_global_init(CURL_GLOBAL_ALL);
        
        // Try up to 3 times with a short delay between attempts
        for (int attempt = 1; attempt <= 3; attempt++) {
            try {
                // Test connection by making a simple API call
                std::string endpoint = "/api/v1/timestamp";
                json response = makeApiCall(endpoint, "", false);
                
                if (response.contains("code") && response["code"] == "200000") {
                    std::cout << "Successfully reconnected to KuCoin" << std::endl;
                    return true;
                }
            } catch (const std::exception& e) {
                std::cerr << "KuCoin reconnect attempt " << attempt << " failed: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        return false;
    } catch (const std::exception& e) {
        std::cerr << "KuCoin reconnect failed: " << e.what() << std::endl;
        return false;
    }
}

// Generate KuCoin API signature - using HMAC-SHA256
std::string KuCoinExchange::generateSignature(const std::string& timestamp, 
                                           const std::string& method,
                                           const std::string& endpoint,
                                           const std::string& body) {
    // Create the string to sign: timestamp + method + endpoint + body
    std::string message = timestamp + method + endpoint + body;
    
    // Prepare the HMAC-SHA256 key and result buffer
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    
    HMAC(EVP_sha256(),
         api_secret_.c_str(), api_secret_.length(),
         reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
         digest, &digest_len);
    
    // Convert to base64
    char base64_digest[1024] = {0};
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64_digest),
                    digest, digest_len);
    
    return std::string(base64_digest);
}

// KuCoin requires the passphrase to be encrypted
std::string KuCoinExchange::getEncryptedPassphrase(const std::string& timestamp) {
    // Encrypt the passphrase using API secret and base64 encode
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    
    HMAC(EVP_sha256(),
         api_secret_.c_str(), api_secret_.length(),
         reinterpret_cast<const unsigned char*>(passphrase_.c_str()), passphrase_.length(),
         digest, &digest_len);
    
    char base64_digest[1024] = {0};
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64_digest),
                    digest, digest_len);
    
    return std::string(base64_digest);
}

// Make API call to KuCoin with option to use futures API
json KuCoinExchange::makeApiCall(const std::string& endpoint, 
                               const std::string& request_body, 
                               bool is_private, 
                               const std::string& method,
                               bool use_futures_api) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response_string;
    std::string url = (use_futures_api ? futures_url_ : base_url_) + endpoint;
    
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
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    // Handle authenticated requests
    if (is_private) {
        // Current UTC timestamp in milliseconds
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::string timestamp = std::to_string(ms);
        
        // Generate signature
        std::string signature = generateSignature(timestamp, method, endpoint, request_body);
        std::string encrypted_passphrase = getEncryptedPassphrase(timestamp);
        
        // Add authentication headers
        headers = curl_slist_append(headers, ("KC-API-KEY: " + api_key_).c_str());
        headers = curl_slist_append(headers, ("KC-API-SIGN: " + signature).c_str());
        headers = curl_slist_append(headers, ("KC-API-TIMESTAMP: " + timestamp).c_str());
        headers = curl_slist_append(headers, ("KC-API-PASSPHRASE: " + encrypted_passphrase).c_str());
        headers = curl_slist_append(headers, "KC-API-KEY-VERSION: 2"); // API key version 2
    }
    
    // Set headers
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    // Set request body for non-GET requests
    if (method != "GET" && !request_body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    }
    
    // Perform request
    CURLcode result = curl_easy_perform(curl);
    
    // Clean up
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (result != CURLE_OK) {
        throw std::runtime_error("CURL request failed: " + std::string(curl_easy_strerror(result)));
    }
    
    // Parse and return JSON response
    try {
        json response = json::parse(response_string);
        return response;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse JSON response: " + std::string(e.what()) + 
                               ", Response: " + response_string);
    }
}

// Update the fee structure from the exchange
void KuCoinExchange::updateFeeStructure() {
    try {
        // Get general fee tier structure
        std::string endpoint = "/api/v1/user-info";
        json response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            // Get user's VIP level
            if (data.contains("level")) {
                fee_structure_.fee_tier = data["level"];
                fee_structure_.tier_name = "VIP " + std::to_string(fee_structure_.fee_tier);
            }
            
            // Get 30-day trading volume if available
            if (data.contains("volumeInBTC")) {
                fee_structure_.volume_30d_usd = std::stod(data["volumeInBTC"].get<std::string>()) * 30000; // Approximate BTC to USD conversion
            }
        }
        
        // Get trading fees for the spot market
        endpoint = "/api/v1/base-fee";
        response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "200000" && response.contains("data")) {
            auto& fee_data = response["data"];
            
            // KuCoin provides fee rates directly as decimal (not percentage)
            if (fee_data.contains("makerFeeRate") && fee_data.contains("takerFeeRate")) {
                fee_structure_.maker_fee = std::stod(fee_data["makerFeeRate"].get<std::string>());
                fee_structure_.taker_fee = std::stod(fee_data["takerFeeRate"].get<std::string>());
                
                // Set spot fees
                fee_structure_.spot_maker_fee = fee_structure_.maker_fee;
                fee_structure_.spot_taker_fee = fee_structure_.taker_fee;
            }
        }
        
        // Get futures trading fees
        endpoint = "/api/v1/contracts/fee-rate";
        response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "200000" && response.contains("data")) {
            auto& futures_data = response["data"];
            
            if (futures_data.contains("makerFeeRate") && futures_data.contains("takerFeeRate")) {
                fee_structure_.perp_maker_fee = std::stod(futures_data["makerFeeRate"].get<std::string>());
                fee_structure_.perp_taker_fee = std::stod(futures_data["takerFeeRate"].get<std::string>());
            } else {
                // Use default spot fees if futures fees aren't available
                fee_structure_.perp_maker_fee = fee_structure_.maker_fee;
                fee_structure_.perp_taker_fee = fee_structure_.taker_fee;
            }
        }
        
        // Margin fees are typically the same as spot on KuCoin
        fee_structure_.margin_maker_fee = fee_structure_.spot_maker_fee;
        fee_structure_.margin_taker_fee = fee_structure_.spot_taker_fee;
        
        // Get withdrawal fees for common coins
        endpoint = "/api/v1/withdrawals/quotas";
        json withdrawals_response = makeApiCall(endpoint, "", true);
        
        if (withdrawals_response["code"] == "200000" && withdrawals_response.contains("data")) {
            auto& currencies = withdrawals_response["data"]["currencies"];
            
            for (const auto& currency : currencies) {
                if (currency.contains("currency") && currency.contains("withdrawalMinFee")) {
                    std::string symbol = currency["currency"];
                    double fee = std::stod(currency["withdrawalMinFee"].get<std::string>());
                    fee_structure_.withdrawal_fees[symbol] = fee;
                }
            }
        } else {
            // Set some common currency withdrawal fees as fallback
            fee_structure_.withdrawal_fees["BTC"] = 0.0005;
            fee_structure_.withdrawal_fees["ETH"] = 0.01;
            fee_structure_.withdrawal_fees["USDT"] = 10.0;
            fee_structure_.withdrawal_fees["USDC"] = 10.0;
        }
        
        std::cout << "Updated KuCoin fee structure: maker=" << fee_structure_.maker_fee 
                 << ", taker=" << fee_structure_.taker_fee << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error updating KuCoin fee structure: " << e.what() << std::endl;
        
        // Set default conservative fees
        fee_structure_.maker_fee = 0.0010;     // 0.10%
        fee_structure_.taker_fee = 0.0015;     // 0.15%
        fee_structure_.spot_maker_fee = 0.0010;
        fee_structure_.spot_taker_fee = 0.0015;
        fee_structure_.perp_maker_fee = 0.0002;
        fee_structure_.perp_taker_fee = 0.0006;
        fee_structure_.margin_maker_fee = 0.0010;
        fee_structure_.margin_taker_fee = 0.0015;
        fee_structure_.fee_tier = 0;
        fee_structure_.tier_name = "Default";
    }
}

std::vector<Candle> KuCoinExchange::getCandles(
    const std::string& symbol, 
    const std::string& interval,
    const std::chrono::system_clock::time_point& start_time,
    const std::chrono::system_clock::time_point& end_time) {
    
    std::vector<Candle> candles;
    
    try {
        // Convert time points to seconds since epoch
        auto start_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            start_time.time_since_epoch()).count();
        auto end_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            end_time.time_since_epoch()).count();
        
        // Map interval string to KuCoin's format
        std::string kucoin_interval = interval;
        if (interval == "1m") kucoin_interval = "1min";
        else if (interval == "5m") kucoin_interval = "5min";
        else if (interval == "15m") kucoin_interval = "15min";
        else if (interval == "30m") kucoin_interval = "30min";
        else if (interval == "1h") kucoin_interval = "1hour";
        else if (interval == "4h") kucoin_interval = "4hour";
        else if (interval == "1d") kucoin_interval = "1day";
        else if (interval == "1w") kucoin_interval = "1week";
        
        // Build query parameters
        std::string endpoint = "/api/v1/market/candles";
        std::string query = "?symbol=" + symbol + 
                           "&type=" + kucoin_interval + 
                           "&startAt=" + std::to_string(start_seconds) + 
                           "&endAt=" + std::to_string(end_seconds);
        
        // Make API call
        json response = makeApiCall(endpoint + query);
        
        // Parse response
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            // KuCoin returns candles in reverse order (newest first)
            // Each candle is an array with: [timestamp, open, close, high, low, volume, turnover]
            for (const auto& candle_data : data) {
                if (candle_data.size() >= 7) {
                    Candle candle;
                    candle.symbol = symbol;
                    
                    // Convert timestamp (seconds) to time_point
                    int64_t timestamp = std::stoll(candle_data[0].get<std::string>());
                    candle.open_time = std::chrono::system_clock::from_time_t(timestamp);
                    
                    // Calculate close time based on interval
                    int64_t interval_seconds = 0;
                    if (kucoin_interval == "1min") interval_seconds = 60;
                    else if (kucoin_interval == "5min") interval_seconds = 300;
                    else if (kucoin_interval == "15min") interval_seconds = 900;
                    else if (kucoin_interval == "30min") interval_seconds = 1800;
                    else if (kucoin_interval == "1hour") interval_seconds = 3600;
                    else if (kucoin_interval == "4hour") interval_seconds = 14400;
                    else if (kucoin_interval == "1day") interval_seconds = 86400;
                    else if (kucoin_interval == "1week") interval_seconds = 604800;
                    
                    candle.close_time = std::chrono::system_clock::from_time_t(timestamp + interval_seconds);
                    
                    // Parse price and volume data
                    candle.open = std::stod(candle_data[1].get<std::string>());
                    candle.close = std::stod(candle_data[2].get<std::string>());
                    candle.high = std::stod(candle_data[3].get<std::string>());
                    candle.low = std::stod(candle_data[4].get<std::string>());
                    candle.volume = std::stod(candle_data[5].get<std::string>());
                    
                    // KuCoin doesn't provide trade count, set to 0
                    candle.trades = 0;
                    
                    candles.push_back(candle);
                }
            }
            
            // Sort candles by time (oldest first)
            std::sort(candles.begin(), candles.end(), 
                     [](const Candle& a, const Candle& b) {
                         return a.open_time < b.open_time;
                     });
        } else {
            std::string error_msg = "Failed to get candles: ";
            if (response.contains("msg")) {
                error_msg += response["msg"].get<std::string>();
            } else {
                error_msg += "Unknown error";
            }
            throw std::runtime_error(error_msg);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::getCandles: " << e.what() << std::endl;
        // Return empty vector on error
    }
    
    return candles;
}

// Market data
std::vector<Instrument> KuCoinExchange::getAvailableInstruments(MarketType type) {
    try {
        json response = makeApiCall("/api/v1/symbols");
        
        if (!response.contains("data")) {
            throw std::runtime_error("Invalid response format: missing 'data' field");
        }
        
        std::vector<Instrument> instruments;
        for (const auto& symbol : response["data"]) {
            try {
                Instrument instrument;
                instrument.symbol = symbol["symbol"].get<std::string>();
                instrument.base_currency = symbol["baseCurrency"].get<std::string>();
                instrument.quote_currency = symbol["quoteCurrency"].get<std::string>();
                
                // Handle both string and number types for price precision
                if (symbol.contains("priceIncrement")) {
                    if (symbol["priceIncrement"].is_string()) {
                        instrument.price_precision = std::stod(symbol["priceIncrement"].get<std::string>());
                    } else {
                        instrument.price_precision = symbol["priceIncrement"].get<double>();
                    }
                }
                
                // Handle both string and number types for amount precision
                if (symbol.contains("baseIncrement")) {
                    if (symbol["baseIncrement"].is_string()) {
                        instrument.qty_precision = std::stod(symbol["baseIncrement"].get<std::string>());
                    } else {
                        instrument.qty_precision = symbol["baseIncrement"].get<double>();
                    }
                }
                
                // Only add active symbols
                if (symbol.contains("enableTrading") && symbol["enableTrading"].get<bool>()) {
                    instruments.push_back(instrument);
                }
            } catch (const std::exception& e) {
                std::cerr << "Error parsing KuCoin symbol: " << e.what() << std::endl;
                continue; // Skip this symbol and continue with others
            }
        }
        
        return instruments;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to get instruments: " + std::string(e.what()));
    }
}

double KuCoinExchange::getPrice(const std::string& symbol) {
    try {
        // Convert symbol format from BTCUSDT to BTC-USDT
        std::string kucoin_symbol = symbol;
        if (symbol.find("-") == std::string::npos) {
            // If the symbol doesn't contain a hyphen, try to insert one
            size_t pos = symbol.find("USDT");
            if (pos != std::string::npos) {
                kucoin_symbol = symbol.substr(0, pos) + "-" + symbol.substr(pos);
            }
        }
        
        // Endpoint for getting ticker information
        std::string endpoint = "/api/v1/market/orderbook/level1?symbol=" + kucoin_symbol;
        
        // Make API call
        json response = makeApiCall(endpoint);
        
        // Parse response
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            if (data.contains("price")) {
                return std::stod(data["price"].get<std::string>());
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
        std::cerr << "Error in KuCoinExchange::getPrice: " << e.what() << std::endl;
        return 0.0;
    }
}

OrderBook KuCoinExchange::getOrderBook(const std::string& symbol, int depth) {
    OrderBook orderbook;
    orderbook.symbol = symbol;
    
    try {
        // Ensure depth is within valid range (KuCoin supports 20, 100)
        int kucoin_depth = 20;
        if (depth > 20) {
            kucoin_depth = 100;
        }
        
        // Endpoint for getting order book
        std::string endpoint = "/api/v1/market/orderbook/level2_" + std::to_string(kucoin_depth) + 
                              "?symbol=" + symbol;
        
        // Make API call
        json response = makeApiCall(endpoint);
        
        // Parse response
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            // Set timestamp
            orderbook.timestamp = std::chrono::system_clock::now();
            
            // Parse bids
            if (data.contains("bids")) {
                for (const auto& bid : data["bids"]) {
                    if (bid.size() >= 2) {
                        PriceLevel level;
                        level.price = std::stod(bid[0].get<std::string>());
                        level.amount = std::stod(bid[1].get<std::string>());
                        orderbook.bids.push_back(level);
                    }
                }
            }
            
            // Parse asks
            if (data.contains("asks")) {
                for (const auto& ask : data["asks"]) {
                    if (ask.size() >= 2) {
                        PriceLevel level;
                        level.price = std::stod(ask[0].get<std::string>());
                        level.amount = std::stod(ask[1].get<std::string>());
                        orderbook.asks.push_back(level);
                    }
                }
            }
            
            // Sort bids in descending order by price
            std::sort(orderbook.bids.begin(), orderbook.bids.end(), 
                     [](const PriceLevel& a, const PriceLevel& b) {
                         return a.price > b.price;
                     });
            
            // Sort asks in ascending order by price
            std::sort(orderbook.asks.begin(), orderbook.asks.end(), 
                     [](const PriceLevel& a, const PriceLevel& b) {
                         return a.price < b.price;
                     });
            
            // Limit to requested depth
            if (orderbook.bids.size() > static_cast<size_t>(depth)) {
                orderbook.bids.resize(depth);
            }
            
            if (orderbook.asks.size() > static_cast<size_t>(depth)) {
                orderbook.asks.resize(depth);
            }
            
        } else {
            std::string error_msg = "Failed to get order book: ";
            if (response.contains("msg")) {
                error_msg += response["msg"].get<std::string>();
            } else {
                error_msg += "Unknown error";
            }
            throw std::runtime_error(error_msg);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::getOrderBook: " << e.what() << std::endl;
        // Return empty orderbook on error
    }
    
    return orderbook;
}

FundingRate KuCoinExchange::getFundingRate(const std::string& symbol) {
    FundingRate rate;
    rate.symbol = symbol;
    
    try {
        // Convert symbol format for KuCoin futures
        // KuCoin uses formats like XBTUSDM for BTC, ETHUSDM for ETH
        std::string kucoin_symbol = symbol;
        if (symbol == "BTCUSDT") {
            kucoin_symbol = "XBTUSDM";
        } else if (symbol.find("USDT") != std::string::npos) {
            // For other symbols, replace USDT with USDM
            kucoin_symbol = symbol.substr(0, symbol.find("USDT")) + "USDM";
        }
        
        // Use the working KuCoin funding rate endpoint
        std::string endpoint = "/api/v1/contract/funding-rates?symbol=" + kucoin_symbol + "&from=" + 
                              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count() - 86400000) + 
                              "&to=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count());
        
        // Make API call to futures API
        json response = makeApiCall(endpoint, "", false, "GET", true);
        
        // Parse response
        if (response.contains("code") && response.at("code") == "200000" && 
            response.contains("data") && response.at("data").is_array() && !response.at("data").empty()) {
            // Get the most recent funding rate (first in the list)
            auto& funding_data = response.at("data").at(0);
            
            // Parse funding rate
            if (funding_data.contains("fundingRate")) {
                // The fundingRate is always returned as a number
                rate.rate = funding_data.at("fundingRate").get<double>();
            } else {
                throw std::runtime_error("Funding rate value not found for symbol: " + symbol);
            }
            
            // Parse funding time
            if (funding_data.contains("timepoint")) {
                // The timepoint is always returned as a number (milliseconds)
                int64_t funding_time_ms = funding_data.at("timepoint").get<int64_t>();
                
                // Set the last funding time
                auto last_funding_time = std::chrono::system_clock::from_time_t(funding_time_ms / 1000);
                
                // Calculate next funding time based on KuCoin's schedule
                rate.next_payment = calculateNextFundingTime();
            } else {
                // Calculate next funding time based on KuCoin's schedule
                rate.next_payment = calculateNextFundingTime();
            }
            
            // KuCoin typically has 8-hour funding intervals
            rate.payment_interval = std::chrono::hours(8);
            
            // For predicted rate, we'll just use the current rate as there's no prediction in the history
            rate.predicted_rate = rate.rate;
            
        } else {
            std::string error_msg = "Failed to get funding rate: ";
            if (response.contains("msg")) {
                error_msg += response.at("msg").get<std::string>();
            } else {
                error_msg += "Unknown error";
            }
            throw std::runtime_error(error_msg);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::getFundingRate: " << e.what() << std::endl;
        
        // Set default values
        rate.rate = 0.0;
        rate.next_payment = calculateNextFundingTime();
        rate.payment_interval = std::chrono::hours(8);
        rate.predicted_rate = 0.0;
    }
    
    return rate;
}

// Calculate the next funding time based on KuCoin's funding schedule (00:00, 08:00, 16:00 UTC)
std::chrono::system_clock::time_point KuCoinExchange::calculateNextFundingTime() {
    // KuCoin funding times occur at 00:00, 08:00, and 16:00 UTC
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

// Fee information
FeeStructure KuCoinExchange::getFeeStructure() {
    // Check if we need to update the fee structure (every 24 hours)
    auto now = std::chrono::system_clock::now();
    if (now - last_fee_update_ > std::chrono::hours(24)) {
        updateFeeStructure();
        last_fee_update_ = now;
    }
    
    return fee_structure_;
}

double KuCoinExchange::getTradingFee(const std::string& symbol, bool is_maker) {
    // Check if we have symbol-specific fees
    auto it = symbol_fees_.find(symbol);
    if (it != symbol_fees_.end()) {
        return is_maker ? it->second.first : it->second.second;
    }
    
    // Otherwise return the general fee based on market type
    if (symbol.find("PERP") != std::string::npos || 
        symbol.find("SWAP") != std::string::npos) {
        // Perpetual futures
        return is_maker ? fee_structure_.perp_maker_fee : fee_structure_.perp_taker_fee;
    } else if (symbol.find("MARGIN") != std::string::npos) {
        // Margin trading
        return is_maker ? fee_structure_.margin_maker_fee : fee_structure_.margin_taker_fee;
    } else {
        // Spot trading (default)
        return is_maker ? fee_structure_.spot_maker_fee : fee_structure_.spot_taker_fee;
    }
}

double KuCoinExchange::getWithdrawalFee(const std::string& currency, double amount) {
    // Check if we have the currency in our fee structure
    auto it = fee_structure_.withdrawal_fees.find(currency);
    if (it != fee_structure_.withdrawal_fees.end()) {
        return it->second;
    }
    
    // If not found, try to fetch the withdrawal fee from the API
    try {
        std::string endpoint = "/api/v1/withdrawals/quotas?currency=" + currency;
        json response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            if (data.contains("withdrawMinFee")) {
                double fee = std::stod(data["withdrawMinFee"].get<std::string>());
                
                // Cache the fee for future use
                fee_structure_.withdrawal_fees[currency] = fee;
                
                return fee;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error fetching withdrawal fee for " << currency << ": " << e.what() << std::endl;
    }
    
    // Return a default conservative estimate if we couldn't get the actual fee
    if (currency == "BTC") return 0.0005;
    if (currency == "ETH") return 0.01;
    if (currency == "USDT") return 10.0;
    if (currency == "USDC") return 10.0;
    
    // For other currencies, return a small percentage of the amount as an estimate
    return amount * 0.001; // 0.1% as a fallback
}

// Account information
AccountBalance KuCoinExchange::getAccountBalance() {
    AccountBalance balance;
    
    try {
        // First get spot account balances
        std::string endpoint = "/api/v1/accounts";
        json response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "200000" && response.contains("data")) {
            auto& accounts = response["data"];
            
            for (const auto& account : accounts) {
                if (account.contains("currency") && account.contains("balance") && account.contains("available") && account.contains("holds")) {
                    std::string currency = account["currency"];
                    double total = std::stod(account["balance"].get<std::string>());
                    double available = std::stod(account["available"].get<std::string>());
                    double locked = std::stod(account["holds"].get<std::string>());
                    
                    balance.total[currency] = total;
                    balance.available[currency] = available;
                    balance.locked[currency] = locked;
                }
            }
        }
        
        // Then get futures account balances
        endpoint = "/api/v1/account-overview";
        response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "200000" && response.contains("data")) {
            auto& futures_data = response["data"];
            
            if (futures_data.contains("currency") && futures_data.contains("accountEquity") && 
                futures_data.contains("availableBalance") && futures_data.contains("orderMargin")) {
                
                std::string currency = futures_data["currency"];
                double total = std::stod(futures_data["accountEquity"].get<std::string>());
                double available = std::stod(futures_data["availableBalance"].get<std::string>());
                double locked = std::stod(futures_data["orderMargin"].get<std::string>());
                
                // Add to existing balance or create new entry
                balance.total[currency + "_FUTURES"] = total;
                balance.available[currency + "_FUTURES"] = available;
                balance.locked[currency + "_FUTURES"] = locked;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::getAccountBalance: " << e.what() << std::endl;
    }
    
    return balance;
}

std::vector<Position> KuCoinExchange::getOpenPositions() {
    std::vector<Position> positions;
    
    try {
        // Get futures positions
        std::string endpoint = "/api/v1/positions";
        json response = makeApiCall(endpoint, "", true);
        
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            for (const auto& position_data : data) {
                if (position_data.contains("symbol") && 
                    position_data.contains("currentQty") && 
                    position_data.contains("avgEntryPrice") && 
                    position_data.contains("liquidationPrice") && 
                    position_data.contains("unrealisedPnl") && 
                    position_data.contains("leverage")) {
                    
                    Position position;
                    position.symbol = position_data["symbol"];
                    position.size = std::stod(position_data["currentQty"].get<std::string>());
                    
                    // Skip positions with zero size
                    if (position.size == 0) continue;
                    
                    position.entry_price = std::stod(position_data["avgEntryPrice"].get<std::string>());
                    
                    // Handle case where liquidation price might be 0 or null
                    if (!position_data["liquidationPrice"].is_null() && 
                        position_data["liquidationPrice"].get<std::string>() != "0") {
                        position.liquidation_price = std::stod(position_data["liquidationPrice"].get<std::string>());
                    } else {
                        position.liquidation_price = 0.0;
                    }
                    
                    position.unrealized_pnl = std::stod(position_data["unrealisedPnl"].get<std::string>());
                    position.leverage = std::stod(position_data["leverage"].get<std::string>());
                    
                    positions.push_back(position);
                }
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::getOpenPositions: " << e.what() << std::endl;
    }
    
    return positions;
}

// Trading operations
std::string KuCoinExchange::placeOrder(const Order& order) {
    try {
        // Determine the appropriate endpoint based on market type
        std::string endpoint;
        std::string request_body;
        
        if (order.symbol.find("PERP") != std::string::npos || 
            order.symbol.find("SWAP") != std::string::npos) {
            // Futures order
            endpoint = "/api/v1/orders";
            
            // Build request body
            json order_json = {
                {"symbol", order.symbol},
                {"side", order.side == OrderSide::BUY ? "buy" : "sell"},
                {"leverage", "1"}, // Default leverage, can be adjusted
                {"type", order.type == OrderType::MARKET ? "market" : "limit"},
                {"size", std::to_string(order.amount)}
            };
            
            // Add price for limit orders
            if (order.type == OrderType::LIMIT) {
                order_json["price"] = std::to_string(order.price);
            }
            
            request_body = order_json.dump();
        } else {
            // Spot order
            endpoint = "/api/v1/orders";
            
            // Build request body
            json order_json = {
                {"symbol", order.symbol},
                {"side", order.side == OrderSide::BUY ? "buy" : "sell"},
                {"type", order.type == OrderType::MARKET ? "market" : "limit"}
            };
            
            // For market orders, specify funds or size based on side
            if (order.type == OrderType::MARKET) {
                if (order.side == OrderSide::BUY) {
                    // For buy market orders, specify funds (quote currency amount)
                    order_json["funds"] = std::to_string(order.amount * order.price);
                } else {
                    // For sell market orders, specify size (base currency amount)
                    order_json["size"] = std::to_string(order.amount);
                }
            } else {
                // For limit orders, specify both price and size
                order_json["price"] = std::to_string(order.price);
                order_json["size"] = std::to_string(order.amount);
            }
            
            request_body = order_json.dump();
        }
        
        // Make API call to place order
        json response = makeApiCall(endpoint, request_body, true, "POST");
        
        // Parse response
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            if (data.contains("orderId")) {
                return data["orderId"];
            }
        }
        
        // If we get here, something went wrong
        std::string error_msg = "Failed to place order: ";
        if (response.contains("msg")) {
            error_msg += response["msg"].get<std::string>();
        } else {
            error_msg += "Unknown error";
        }
        throw std::runtime_error(error_msg);
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::placeOrder: " << e.what() << std::endl;
        return ""; // Return empty string on error
    }
}

bool KuCoinExchange::cancelOrder(const std::string& order_id) {
    try {
        // Endpoint for canceling an order
        std::string endpoint = "/api/v1/orders/" + order_id;
        
        // Make API call to cancel order
        json response = makeApiCall(endpoint, "", true, "DELETE");
        
        // Parse response
        if (response["code"] == "200000") {
            return true;
        } else {
            std::string error_msg = "Failed to cancel order: ";
            if (response.contains("msg")) {
                error_msg += response["msg"].get<std::string>();
            } else {
                error_msg += "Unknown error";
            }
            throw std::runtime_error(error_msg);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::cancelOrder: " << e.what() << std::endl;
        return false; // Return false on error
    }
}

OrderStatus KuCoinExchange::getOrderStatus(const std::string& order_id) {
    try {
        // Endpoint for getting order details
        std::string endpoint = "/api/v1/orders/" + order_id;
        
        // Make API call
        json response = makeApiCall(endpoint, "", true);
        
        // Parse response
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            if (data.contains("isActive")) {
                bool is_active = data["isActive"];
                
                if (is_active) {
                    // Order is still active
                    if (data.contains("dealSize") && std::stod(data["dealSize"].get<std::string>()) > 0) {
                        return OrderStatus::PARTIALLY_FILLED;
                    } else {
                        return OrderStatus::NEW;
                    }
                } else {
                    // Order is not active
                    if (data.contains("cancelExist") && data["cancelExist"]) {
                        return OrderStatus::CANCELED;
                    } else {
                        return OrderStatus::FILLED;
                    }
                }
            }
        }
        
        // If we get here, something went wrong
        std::string error_msg = "Failed to get order status: ";
        if (response.contains("msg")) {
            error_msg += response["msg"].get<std::string>();
        } else {
            error_msg += "Unknown error";
        }
        throw std::runtime_error(error_msg);
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::getOrderStatus: " << e.what() << std::endl;
        return OrderStatus::REJECTED;  // Changed from UNKNOWN to REJECTED
    }
}

// Historical data
std::vector<Trade> KuCoinExchange::getRecentTrades(const std::string& symbol, int limit) {
    std::vector<Trade> trades;
    
    try {
        // Ensure limit is within valid range
        int kucoin_limit = std::min(limit, 100); // KuCoin max limit is 100
        
        // Endpoint for getting recent trades
        std::string endpoint = "/api/v1/market/histories?symbol=" + symbol + 
                              "&limit=" + std::to_string(kucoin_limit);
        
        // Make API call
        json response = makeApiCall(endpoint);
        
        // Parse response
        if (response["code"] == "200000" && response.contains("data")) {
            auto& data = response["data"];
            
            for (const auto& trade_data : data) {
                if (trade_data.contains("time") && 
                    trade_data.contains("price") && 
                    trade_data.contains("size") && 
                    trade_data.contains("side")) {
                    
                    Trade trade;
                    trade.symbol = symbol;
                    trade.price = std::stod(trade_data["price"].get<std::string>());
                    trade.amount = std::stod(trade_data["size"].get<std::string>());
                    trade.side = trade_data["side"];
                    
                    // Convert timestamp (milliseconds) to time_point
                    int64_t timestamp_ms = std::stoll(trade_data["time"].get<std::string>());
                    trade.timestamp = std::chrono::system_clock::from_time_t(timestamp_ms / 1000) + 
                                     std::chrono::milliseconds(timestamp_ms % 1000);
                    
                    // Set trade ID if available
                    if (trade_data.contains("tradeId")) {
                        trade.trade_id = trade_data["tradeId"];
                    } else if (trade_data.contains("sequence")) {
                        trade.trade_id = trade_data["sequence"];
                    } else {
                        // Generate a unique ID based on timestamp and other data
                        trade.trade_id = std::to_string(timestamp_ms) + "_" + 
                                        std::to_string(trade.price) + "_" + 
                                        std::to_string(trade.amount);
                    }
                    
                    trades.push_back(trade);
                }
            }
        } else {
            std::string error_msg = "Failed to get recent trades: ";
            if (response.contains("msg")) {
                error_msg += response["msg"].get<std::string>();
            } else {
                error_msg += "Unknown error";
            }
            throw std::runtime_error(error_msg);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in KuCoinExchange::getRecentTrades: " << e.what() << std::endl;
    }
    
    return trades;
}

} // namespace funding

// Factory function implementation
std::shared_ptr<funding::ExchangeInterface> funding::createKuCoinExchange(const funding::ExchangeConfig& config) {
    return std::make_shared<funding::KuCoinExchange>(config);
}
