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
    BinanceExchange(const ExchangeConfig& config) : 
        api_key_(config.getApiKey()),
        api_secret_(config.getApiSecret()),
        base_url_("https://api.binance.com"),
        use_testnet_(config.getUseTestnet()),
        last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
        
        if (use_testnet_) {
            base_url_ = "https://testnet.binance.vision";
        }
        
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
        
        try {
            if (type == MarketType::SPOT || type == MarketType::PERPETUAL) {
                for (const auto& symbol : response["symbols"]) {
                    if (symbol["status"] == "TRADING") {
                        Instrument instrument;
                        instrument.symbol = symbol["symbol"];
                        instrument.base_currency = symbol["baseAsset"];
                        instrument.quote_currency = symbol["quoteAsset"];
                        instrument.market_type = type;
                        instrument.min_qty = std::stod(symbol["filters"][1]["minQty"].get<std::string>());
                        instrument.max_qty = std::stod(symbol["filters"][1]["maxQty"].get<std::string>());
                        instrument.price_precision = symbol["quotePrecision"];
                        instrument.qty_precision = symbol["baseAssetPrecision"];
                        instruments.push_back(instrument);
                    }
                }
            } else if (type == MarketType::MARGIN) {
                for (const auto& pair : response) {
                    if (pair["status"] == "NORMAL") {
                        Instrument instrument;
                        instrument.symbol = pair["symbol"];
                        instrument.base_currency = pair["base"];
                        instrument.quote_currency = pair["quote"];
                        instrument.market_type = type;
                        instrument.min_qty = 0.0; // Would need additional API call
                        instrument.max_qty = 0.0; // Would need additional API call
                        instrument.price_precision = 8; // Default
                        instrument.qty_precision = 8; // Default
                        instruments.push_back(instrument);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Binance instruments: " << e.what() << std::endl;
        }
        
        return instruments;
    }
    
    double getPrice(const std::string& symbol) override {
        std::string endpoint = "/api/v3/ticker/price?symbol=" + symbol;
        json response = makeApiCall(endpoint, "", false);
        
        return std::stod(response["price"].get<std::string>());
    }
    
    OrderBook getOrderBook(const std::string& symbol, int depth) override {
        std::string endpoint = "/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(depth);
        json response = makeApiCall(endpoint, "", false);
        
        OrderBook book;
        
        for (const auto& bid : response["bids"]) {
            OrderBookLevel level;
            level.price = std::stod(bid[0].get<std::string>());
            level.amount = std::stod(bid[1].get<std::string>());
            book.bids.push_back(level);
        }
        
        for (const auto& ask : response["asks"]) {
            OrderBookLevel level;
            level.price = std::stod(ask[0].get<std::string>());
            level.amount = std::stod(ask[1].get<std::string>());
            book.asks.push_back(level);
        }
        
        return book;
    }
    
    FundingRate getFundingRate(const std::string& symbol) override {
        std::string endpoint = "/fapi/v1/premiumIndex?symbol=" + symbol;
        json response = makeApiCall(endpoint, "", false);
        
        FundingRate rate;
        rate.rate = std::stod(response["lastFundingRate"].get<std::string>());
        rate.payment_interval = std::chrono::hours(8); // Binance pays funding every 8 hours
        rate.next_payment = std::chrono::system_clock::from_time_t(
            response["nextFundingTime"].get<int64_t>() / 1000);
        
        return rate;
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
        // Implementation would go here
        return AccountBalance();
    }
    
    std::vector<Position> getOpenPositions() override {
        // Implementation would go here
        return std::vector<Position>();
    }
    
    std::string placeOrder(const Order& order) override {
        // Implementation would go here
        return "order-id-placeholder";
    }
    
    bool cancelOrder(const std::string& order_id) override {
        // Implementation would go here
        return true;
    }
    
    OrderStatus getOrderStatus(const std::string& order_id) override {
        // Implementation would go here
        return OrderStatus::FILLED;
    }
    
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override {
        // Implementation would go here
        return std::vector<Trade>();
    }
    
    std::vector<Candle> getCandles(const std::string& symbol, 
                                 const std::string& interval,
                                 const std::chrono::system_clock::time_point& start,
                                 const std::chrono::system_clock::time_point& end) override {
        // Implementation would go here
        return std::vector<Candle>();
    }
    
    bool isConnected() override {
        // Implementation would go here
        return true;
    }
    
    bool reconnect() override {
        // Implementation would go here
        return true;
    }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string base_url_;
    bool use_testnet_;
    FeeStructure fee_structure_;
    std::map<std::string, std::pair<double, double>> symbol_fees_; // symbol -> (maker, taker)
    std::chrono::system_clock::time_point last_fee_update_;
    
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
    json makeApiCall(const std::string& endpoint, std::string params = "", bool is_private = false) {
        CURL* curl = curl_easy_init();
        std::string response_string;
        std::string url = base_url_ + endpoint;
        
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
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
            params += "&signature=" + signature;
        }
        
        if (!params.empty()) {
            url += "?" + params;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        
        // Add API key header for authenticated requests
        if (is_private) {
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        
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
            // Get trading fee information - timestamp and signature will be added by makeApiCall
            json fee_info = makeApiCall("/api/v3/account", "", true);
            
            // Parse maker/taker fees
            fee_structure_.maker_fee = std::stod(fee_info["makerCommission"].get<std::string>()) / 10000.0;
            fee_structure_.taker_fee = std::stod(fee_info["takerCommission"].get<std::string>()) / 10000.0;
            
            // Parse VIP tier info 
            fee_structure_.fee_tier = fee_info["commissionRates"]["tier"].get<int>();
            fee_structure_.volume_30d_usd = fee_info["commissionRates"]["30dVolume"].get<double>();
            
            // Symbol-specific fees
            json symbol_fees = makeApiCall("/sapi/v1/asset/tradeFee", "", true);
            for (const auto& symbol_fee : symbol_fees) {
                std::string symbol = symbol_fee["symbol"];
                double maker = std::stod(symbol_fee["makerCommission"].get<std::string>());
                double taker = std::stod(symbol_fee["takerCommission"].get<std::string>());
                symbol_fees_[symbol] = std::make_pair(maker, taker);
            }
            
            // Get withdrawal fees
            json coins_info = makeApiCall("/sapi/v1/capital/config/getall", "", true);
            for (const auto& coin : coins_info) {
                std::string currency = coin["coin"];
                double withdraw_fee = std::stod(coin["withdrawFee"].get<std::string>());
                fee_structure_.withdrawal_fees[currency] = withdraw_fee;
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