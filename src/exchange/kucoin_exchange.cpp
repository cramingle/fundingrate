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
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
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
    std::string api_passphrase_;
    std::string base_url_;
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
    
    // Make API call to KuCoin
    json makeApiCall(const std::string& endpoint, 
                    const std::string& request_body = "", 
                    bool is_private = false, 
                    const std::string& method = "GET");
    
    // Update the fee structure from the exchange
    void updateFeeStructure();
};

// Constructor
KuCoinExchange::KuCoinExchange(const ExchangeConfig& config) : 
    api_key_(config.getApiKey()),
    api_secret_(config.getApiSecret()),
    api_passphrase_(config.getParam("passphrase")),  // KuCoin requires a passphrase
    base_url_("https://api.kucoin.com"),
    use_testnet_(config.getUseTestnet()),
    last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
    
    if (use_testnet_) {
        base_url_ = "https://openapi-sandbox.kucoin.com"; // Sandbox API URL
    }
    
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
        // Make a simple API call to test connectivity
        std::string endpoint = "/api/v1/timestamp";
        makeApiCall(endpoint, "", false);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "KuCoin connection check failed: " << e.what() << std::endl;
        return false;
    }
}

bool KuCoinExchange::reconnect() {
    try {
        curl_global_cleanup();
        curl_global_init(CURL_GLOBAL_ALL);
        
        // Test the connection after re-initializing
        return isConnected();
    } catch (const std::exception& e) {
        std::cerr << "KuCoin reconnection failed: " << e.what() << std::endl;
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
         reinterpret_cast<const unsigned char*>(api_passphrase_.c_str()), api_passphrase_.length(),
         digest, &digest_len);
    
    char base64_digest[1024] = {0};
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64_digest),
                    digest, digest_len);
    
    return std::string(base64_digest);
}

// Make API call to KuCoin
json KuCoinExchange::makeApiCall(const std::string& endpoint, 
                               const std::string& request_body, 
                               bool is_private, 
                               const std::string& method) {
    CURL* curl = curl_easy_init();
    std::string response_string;
    std::string url = base_url_ + endpoint;
    
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    
    // Set request method
    if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    
    // Prepare headers
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

} // namespace funding

// Factory function implementation
std::shared_ptr<funding::ExchangeInterface> funding::createKuCoinExchange(const funding::ExchangeConfig& config) {
    return std::make_shared<funding::KuCoinExchange>(config);
}
