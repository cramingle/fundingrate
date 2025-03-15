#include <exchange/exchange_interface.h>
#include <config/config_manager.h>
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

    BinanceExchange(const ExchangeConfig& config) 
        : api_key_(config.api_key), 
          api_secret_(config.api_secret),
          base_url_(config.base_url.empty() ? "https://api.binance.com" : config.base_url),
          connect_timeout_ms_(config.connect_timeout_ms > 0 ? config.connect_timeout_ms : 5000),
          request_timeout_ms_(config.request_timeout_ms > 0 ? config.request_timeout_ms : 10000),
          use_testnet_(config.use_testnet),
          last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
        
        // Initialize CURL globally if not already done
        static bool curl_initialized = false;
        if (!curl_initialized) {
            curl_global_init(CURL_GLOBAL_ALL);
            curl_initialized = true;
        }
        
        // Initialize price cache
        price_cache_.clear();
        
        // Initialize fee structure
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
        
        try {
            std::string endpoint;
            json response;
            
            if (type == MarketType::SPOT) {
                endpoint = "/api/v3/exchangeInfo";
                std::cout << "Fetching Binance spot instruments from endpoint: " << endpoint << std::endl;
                response = makeApiCall(endpoint);
            } else if (type == MarketType::PERPETUAL) {
                // Use the futures API for perpetual contracts
                std::string original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
                
                endpoint = "/fapi/v1/exchangeInfo";
                std::cout << "Fetching Binance perpetual instruments from endpoint: " << endpoint << std::endl;
                try {
                    response = makeApiCall(endpoint);
                } catch (const std::exception& e) {
                    // Restore original base URL before re-throwing
                    base_url_ = original_base_url;
                    std::cerr << "Error fetching Binance instruments: " << e.what() << std::endl;
                    return instruments;
                }
                
                // Restore original base URL
                base_url_ = original_base_url;
            } else {
                std::cerr << "Unsupported market type for Binance: " << static_cast<int>(type) << std::endl;
                return instruments;
            }
            
            // Check if response is empty or null
            if (response.empty() || response.is_null()) {
                std::cerr << "Empty or null response received from Binance API" << std::endl;
                return instruments;
            }
            
            if (type == MarketType::SPOT || type == MarketType::PERPETUAL) {
                // Check if the response contains the symbols field
                if (!response.contains("symbols")) {
                    std::cerr << "Invalid response format: missing 'symbols' field" << std::endl;
                    if (!response.empty()) {
                        std::cerr << "Response: " << response.dump().substr(0, 1000) << "..." << std::endl;
                    }
                    return instruments;
                }
                
                if (!response["symbols"].is_array()) {
                    std::cerr << "Invalid response format: 'symbols' is not an array" << std::endl;
                    if (!response.empty()) {
                        std::cerr << "Response: " << response.dump().substr(0, 1000) << "..." << std::endl;
                    }
                    return instruments;
                }
                
                std::cout << "Processing " << response["symbols"].size() << " symbols from Binance" << std::endl;
                
                for (const auto& symbol : response["symbols"]) {
                    try {
                        // Skip symbols that are not trading
                        if (!symbol.contains("status")) {
                            std::cerr << "Symbol missing 'status' field, skipping" << std::endl;
                            continue;
                        }
                        
                        if (symbol["status"] != "TRADING") {
                            // Skip non-trading symbols
                            continue;
                        }
                        
                        if (!symbol.contains("symbol")) {
                            std::cerr << "Symbol missing 'symbol' field, skipping" << std::endl;
                            continue;
                        }
                        
                        Instrument instrument;
                        instrument.symbol = symbol["symbol"].get<std::string>();
                        
                        // Handle different field names between spot and futures
                        if (type == MarketType::SPOT) {
                            if (!symbol.contains("baseAsset")) {
                                std::cerr << "Symbol " << instrument.symbol << " missing 'baseAsset' field, skipping" << std::endl;
                                continue;
                            }
                            
                            if (!symbol.contains("quoteAsset")) {
                                std::cerr << "Symbol " << instrument.symbol << " missing 'quoteAsset' field, skipping" << std::endl;
                                continue;
                            }
                            
                            instrument.base_currency = symbol["baseAsset"].get<std::string>();
                            instrument.quote_currency = symbol["quoteAsset"].get<std::string>();
                        } else {
                            // For futures, extract base and quote from the symbol (e.g., BTCUSDT)
                            std::string sym = symbol["symbol"].get<std::string>();
                            
                            // Check if the symbol contains "USDT"
                            size_t pos = sym.find("USDT");
                            if (pos != std::string::npos && pos > 0) {
                                instrument.base_currency = sym.substr(0, pos);
                                instrument.quote_currency = "USDT";
                            } else {
                                // Try other quote currencies
                                pos = sym.find("BUSD");
                                if (pos != std::string::npos && pos > 0) {
                                    instrument.base_currency = sym.substr(0, pos);
                                    instrument.quote_currency = "BUSD";
                                } else {
                                    pos = sym.find("USDC");
                                    if (pos != std::string::npos && pos > 0) {
                                        instrument.base_currency = sym.substr(0, pos);
                                        instrument.quote_currency = "USDC";
                                    } else {
                                        // Default fallback - use the symbol as is
                                        instrument.base_currency = sym;
                                        instrument.quote_currency = "UNKNOWN";
                                        std::cerr << "Could not determine base/quote for futures symbol: " << sym << std::endl;
                                    }
                                }
                            }
                            
                            // Log the extracted base and quote currencies
                            std::cout << "Futures symbol: " << sym 
                                      << ", Base: " << instrument.base_currency 
                                      << ", Quote: " << instrument.quote_currency << std::endl;
                        }
                        
                        instrument.market_type = type;
                        
                        // Set default values in case filters are missing
                        instrument.min_qty = 0.00001;
                        instrument.max_qty = 9999999.0;
                        instrument.tick_size = 0.00000001;
                        instrument.price_precision = 8;
                        instrument.qty_precision = 8;
                        
                        // Extract filters for min/max quantity and price precision
                        if (symbol.contains("filters") && symbol["filters"].is_array()) {
                            for (const auto& filter : symbol["filters"]) {
                                try {
                                    if (!filter.contains("filterType")) {
                                        continue;
                                    }
                                    
                                    if (filter["filterType"] == "LOT_SIZE") {
                                        if (filter.contains("minQty") && filter["minQty"].is_string()) {
                                            instrument.min_qty = std::stod(filter["minQty"].get<std::string>());
                                        }
                                        
                                        if (filter.contains("maxQty") && filter["maxQty"].is_string()) {
                                            instrument.max_qty = std::stod(filter["maxQty"].get<std::string>());
                                        }
                                    } else if (filter["filterType"] == "PRICE_FILTER") {
                                        if (filter.contains("tickSize") && filter["tickSize"].is_string()) {
                                            instrument.tick_size = std::stod(filter["tickSize"].get<std::string>());
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    std::cerr << "Error processing filter for " << instrument.symbol << ": " << e.what() << std::endl;
                                    // Continue with next filter
                                }
                            }
                        }
                        
                        // Set precision values
                        try {
                            if (symbol.contains("quotePrecision")) {
                                instrument.price_precision = symbol["quotePrecision"].get<int>();
                            }
                            
                            if (symbol.contains("baseAssetPrecision")) {
                                instrument.qty_precision = symbol["baseAssetPrecision"].get<int>();
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Error setting precision for " << instrument.symbol << ": " << e.what() << std::endl;
                            // Use default values set earlier
                        }
                        
                        instruments.push_back(instrument);
                    } catch (const json::type_error& e) {
                        std::cerr << "JSON type error parsing instrument: " << e.what() << std::endl;
                        // Continue with next instrument
                    } catch (const std::exception& e) {
                        std::cerr << "Error parsing instrument: " << e.what() << std::endl;
                        // Continue with next instrument
                    }
                }
            }
            
            std::cout << "Found " << instruments.size() << " instruments for market type " 
                     << static_cast<int>(type) << " on Binance" << std::endl;
            
            return instruments;
        } catch (const json::type_error& e) {
            std::cerr << "JSON type error fetching Binance instruments: " << e.what() << std::endl;
            return instruments;
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Binance instruments: " << e.what() << std::endl;
            return instruments;
        } catch (...) {
            std::cerr << "Unknown error fetching Binance instruments" << std::endl;
            return instruments;
        }
    }
    
    double getPrice(const std::string& symbol) override {
        try {
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
            // First check if it contains "PERP" in the name
            bool is_futures = upper_symbol.find("PERP") != std::string::npos;
            
            // If not, check if it ends with common futures suffixes
            if (!is_futures) {
                is_futures = upper_symbol.find("USDT") != std::string::npos ||
                             upper_symbol.find("BUSD") != std::string::npos ||
                             upper_symbol.find("USDC") != std::string::npos;
            }
            
            std::cout << "Symbol: " << upper_symbol << ", is_futures: " << (is_futures ? "true" : "false") << std::endl;
            
            // Save original base URL if using futures API
            std::string original_base_url;
            if (is_futures) {
                original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
            }
            
            // Set endpoint and parameters
            std::string endpoint = is_futures ? "/fapi/v1/ticker/price" : "/api/v3/ticker/price";
            std::string params = "symbol=" + upper_symbol;
            
            // Make API call
            json response;
            try {
                response = makeApiCall(endpoint, params, false, "GET", is_futures);
            } catch (const std::exception& e) {
                // Restore original base URL if needed
                if (is_futures) {
                    base_url_ = original_base_url;
                }
                
                // If we failed with futures API, try with spot API
                if (is_futures) {
                    std::cout << "Failed to get price with futures API, trying spot API for " << upper_symbol << std::endl;
                    is_futures = false;
                    endpoint = "/api/v3/ticker/price";
                    try {
                        response = makeApiCall(endpoint, params, false, "GET", false);
                    } catch (const std::exception& e) {
                        std::cerr << "Error fetching price for " << symbol << " with spot API: " << e.what() << std::endl;
                        throw std::runtime_error("Failed to get price for " + symbol + " with both futures and spot APIs");
                    }
                } else {
                    throw;
                }
            }
            
            // Restore original base URL if needed
            if (is_futures) {
                base_url_ = original_base_url;
            }
            
            // Check if the response contains the price field
            if (!response.contains("price")) {
                std::cerr << "Price not found in response for symbol: " << symbol << std::endl;
                if (!response.empty()) {
                    std::cerr << "Response: " << response.dump() << std::endl;
                }
                throw std::runtime_error("Price not found in response for symbol: " + symbol);
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
            // Convert symbol to uppercase for consistency
            std::string upper_symbol = symbol;
            std::transform(upper_symbol.begin(), upper_symbol.end(), upper_symbol.begin(), ::toupper);
            
            // Determine if this is a futures symbol
            bool is_futures = upper_symbol.find("PERP") != std::string::npos;
            
            // Save original base URL if using futures API
            std::string original_base_url;
            if (is_futures) {
                original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
            }
            
            // Set endpoint based on market type
            std::string endpoint;
            if (is_futures) {
                endpoint = "/fapi/v1/depth?symbol=" + upper_symbol + "&limit=" + std::to_string(depth);
            } else {
                endpoint = "/api/v3/depth?symbol=" + upper_symbol + "&limit=" + std::to_string(depth);
            }
            
            // Make API call
            json response;
            try {
                response = makeApiCall(endpoint, "", false, "GET", is_futures);
            } catch (const std::exception& e) {
                // Restore original base URL if needed
                if (is_futures) {
                    base_url_ = original_base_url;
                }
                std::cerr << "Error fetching order book for " << symbol << ": " << e.what() << std::endl;
                return book;
            }
            
            // Restore original base URL if needed
            if (is_futures) {
                base_url_ = original_base_url;
            }
            
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
            } else {
                std::cerr << "Missing or invalid 'bids' field in order book response for " << symbol << std::endl;
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
            } else {
                std::cerr << "Missing or invalid 'asks' field in order book response for " << symbol << std::endl;
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
            std::cout << "Getting funding rate for " << symbol << " on Binance" << std::endl;
            
            // Binance futures API is on a different base URL
            std::string original_base_url = base_url_;
            base_url_ = "https://fapi.binance.com";
            
            std::string endpoint = "/fapi/v1/premiumIndex?symbol=" + symbol;
            json response;
            
            try {
                // Use the futures_api parameter to indicate this is a futures API call
                std::cout << "Making API call to " << endpoint << std::endl;
                response = makeApiCall(endpoint, "", false, "GET", true);
                std::cout << "API call successful" << std::endl;
            } catch (const std::exception& e) {
                // Restore original base URL before re-throwing
                base_url_ = original_base_url;
                std::cerr << "Error fetching funding rate for " << symbol << ": " << e.what() << std::endl;
                
                // Set default values on error
                funding.rate = 0.0;
                funding.next_payment = calculateNextFundingTime();
                funding.payment_interval = std::chrono::hours(8);
                funding.predicted_rate = 0.0;
                return funding;
            }
            
            // Restore original base URL
            base_url_ = original_base_url;
            
            // Check if the response is empty or null
            if (response.empty() || response.is_null()) {
                std::cerr << "Empty or null response when fetching funding rate for " << symbol << std::endl;
                
                // Set default values
                funding.rate = 0.0;
                funding.next_payment = calculateNextFundingTime();
                funding.payment_interval = std::chrono::hours(8);
                funding.predicted_rate = 0.0;
                return funding;
            }
            
            // Check if the response is an error message
            if (response.is_object() && response.contains("code") && response.contains("msg")) {
                std::cerr << "API error when fetching funding rate for " << symbol 
                          << ": Code " << response["code"].get<int>() 
                          << ", Message: " << response["msg"].get<std::string>() << std::endl;
                
                // Set default values
                funding.rate = 0.0;
                funding.next_payment = calculateNextFundingTime();
                funding.payment_interval = std::chrono::hours(8);
                funding.predicted_rate = 0.0;
                return funding;
            }
            
            // Dump the response for debugging (truncated for large responses)
            std::cout << "Response for " << symbol << ": " 
                      << (response.dump().length() > 1000 ? response.dump().substr(0, 1000) + "..." : response.dump()) 
                      << std::endl;
            
            try {
                // Check if the response contains the lastFundingRate field
                if (response.contains("lastFundingRate")) {
                    std::cout << "Found lastFundingRate field" << std::endl;
                    
                    // Check if lastFundingRate is a string
                    if (response["lastFundingRate"].is_string()) {
                        funding.rate = std::stod(response["lastFundingRate"].get<std::string>());
                    } else if (response["lastFundingRate"].is_number()) {
                        funding.rate = response["lastFundingRate"].get<double>();
                    } else {
                        std::cerr << "lastFundingRate is neither a string nor a number" << std::endl;
                        funding.rate = 0.0;
                    }
                    
                    std::cout << "Funding rate: " << funding.rate << std::endl;
                    
                    // Get next funding time
                    if (response.contains("nextFundingTime")) {
                        try {
                            int64_t next_funding_time = 0;
                            
                            if (response["nextFundingTime"].is_string()) {
                                next_funding_time = std::stoll(response["nextFundingTime"].get<std::string>());
                            } else if (response["nextFundingTime"].is_number()) {
                                next_funding_time = response["nextFundingTime"].get<int64_t>();
                            } else {
                                std::cerr << "nextFundingTime is neither a string nor a number" << std::endl;
                                next_funding_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count() + 8 * 60 * 60 * 1000;
                            }
                            
                            // Convert milliseconds to time_point
                            funding.next_payment = std::chrono::system_clock::from_time_t(next_funding_time / 1000);
                        } catch (const std::exception& e) {
                            std::cerr << "Error parsing nextFundingTime: " << e.what() << std::endl;
                            funding.next_payment = calculateNextFundingTime();
                        }
                    } else {
                        // Calculate next funding time based on Binance's schedule
                        std::cerr << "nextFundingTime field not found" << std::endl;
                        funding.next_payment = calculateNextFundingTime();
                    }
                    
                    // Binance has 8-hour funding intervals
                    funding.payment_interval = std::chrono::hours(8);
                    
                    // Try to get predicted rate if available
                    if (response.contains("predictedFundingRate")) {
                        try {
                            if (response["predictedFundingRate"].is_string()) {
                                funding.predicted_rate = std::stod(response["predictedFundingRate"].get<std::string>());
                            } else if (response["predictedFundingRate"].is_number()) {
                                funding.predicted_rate = response["predictedFundingRate"].get<double>();
                            } else {
                                std::cerr << "predictedFundingRate is neither a string nor a number" << std::endl;
                                funding.predicted_rate = funding.rate; // Use current rate as fallback
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "Error parsing predictedFundingRate: " << e.what() << std::endl;
                            funding.predicted_rate = funding.rate; // Use current rate as fallback
                        }
                    } else {
                        // Use current rate as predicted rate if not provided
                        funding.predicted_rate = funding.rate;
                    }
                } else {
                    // Log the error and provide default values
                    std::cerr << "Warning: Funding rate data not found for symbol: " << symbol << std::endl;
                    if (!response.empty()) {
                        std::cerr << "Response: " << response.dump() << std::endl;
                    }
                    
                    // Set default values
                    funding.rate = 0.0;
                    funding.next_payment = calculateNextFundingTime();
                    funding.payment_interval = std::chrono::hours(8);
                    funding.predicted_rate = 0.0;
                }
            } catch (const json::type_error& e) {
                std::cerr << "JSON type error when parsing funding rate for " << symbol << ": " << e.what() << std::endl;
                
                // Set default values
                funding.rate = 0.0;
                funding.next_payment = calculateNextFundingTime();
                funding.payment_interval = std::chrono::hours(8);
                funding.predicted_rate = 0.0;
            } catch (const std::exception& e) {
                std::cerr << "Error parsing funding rate for " << symbol << ": " << e.what() << std::endl;
                
                // Set default values
                funding.rate = 0.0;
                funding.next_payment = calculateNextFundingTime();
                funding.payment_interval = std::chrono::hours(8);
                funding.predicted_rate = 0.0;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in getFundingRate for " << symbol << ": " << e.what() << std::endl;
            
            // Set default values
            funding.rate = 0.0;
            funding.next_payment = calculateNextFundingTime();
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
    int connect_timeout_ms_;
    int request_timeout_ms_;
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
    json makeApiCall(const std::string& endpoint, std::string params = "", bool auth = false, 
                    const std::string& method = "GET", bool futures_api = false) {
        std::cout << "Making API call to " << (futures_api ? "https://fapi.binance.com" : base_url_) 
                  << endpoint << " with params: " << params << std::endl;
        
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "Failed to initialize CURL" << std::endl;
            return json::object(); // Return empty JSON object
        }
        
        std::string url;
        
        // Check if this is a futures API call
        if (futures_api) {
            // Use the futures API URL
            url = "https://fapi.binance.com" + endpoint;
        } else {
            // Use the configured base URL
            url = base_url_ + endpoint;
        }
        
        // For private API calls, add timestamp and signature
        if (auth) {
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
            
            // Generate HMAC-SHA256 signature
            unsigned char* digest = HMAC(EVP_sha256(), api_secret_.c_str(), api_secret_.length(),
                                        (unsigned char*)params.c_str(), params.length(), NULL, NULL);
            
            // Convert to hex string
            char signature[65];
            for (int i = 0; i < 32; i++) {
                sprintf(&signature[i*2], "%02x", digest[i]);
            }
            signature[64] = 0;
            
            // Add signature to params
            params += "&signature=" + std::string(signature);
        }
        
        // Append parameters to URL for GET requests
        if (method == "GET" && !params.empty()) {
            url += "?" + params;
        }
        
        std::cout << "Final URL: " << url << std::endl;
        
        // Set up CURL request
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        
        // Response string to store result
        std::string response_string;
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        
        // Set timeouts
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms_);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request_timeout_ms_);
        
        // Add API key header for authenticated requests
        struct curl_slist* headers = NULL;
        if (auth) {
            headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
            
            // Add content-type header for POST requests
            if (method == "POST") {
                headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
            }
            
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        
        // Set request method
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!params.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params.c_str());
            }
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }
        
        // Perform the request
        CURLcode res = curl_easy_perform(curl);
        
        // Check for errors
        if (res != CURLE_OK) {
            std::string error_msg = "CURL request failed: " + std::string(curl_easy_strerror(res));
            std::cerr << error_msg << std::endl;
            
            // Clean up
            if (headers) {
                curl_slist_free_all(headers);
            }
            curl_easy_cleanup(curl);
            
            // Return empty JSON object instead of throwing
            return json::object();
        }
        
        // Get HTTP response code
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        
        // Clean up
        if (headers) {
            curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);
        
        // Check for empty response
        if (response_string.empty()) {
            std::cerr << "Empty response from API" << std::endl;
            return json::object(); // Return empty JSON object
        }
        
        // Print response (truncated for large responses)
        std::cout << "Response: " << (response_string.length() > 1000 ? response_string.substr(0, 1000) + "..." : response_string) << std::endl;
        
        // Parse JSON response with enhanced error handling
        json response_json = json::object(); // Default to empty object
        try {
            // Check if the response is valid JSON before parsing
            if (response_string[0] != '{' && response_string[0] != '[') {
                std::cerr << "Response is not valid JSON: " << response_string.substr(0, 100) << std::endl;
                return json::object(); // Return empty JSON object
            }
            
            // Parse the JSON response
            response_json = json::parse(response_string);
            
            // Check for API error response
            if (response_json.is_object()) {
                if (response_json.contains("code") && response_json.contains("msg")) {
                    // This is likely an error response
                    std::cerr << "API error: Code " << response_json["code"].get<int>() 
                              << ", Message: " << response_json["msg"].get<std::string>() << std::endl;
                }
            }
        } catch (const json::parse_error& e) {
            std::cerr << "JSON parsing error: " << e.what() << std::endl;
            std::cerr << "Response string: " << response_string.substr(0, 1000) << (response_string.length() > 1000 ? "..." : "") << std::endl;
            // Return empty JSON object instead of throwing
            return json::object();
        } catch (const json::type_error& e) {
            std::cerr << "JSON type error: " << e.what() << std::endl;
            std::cerr << "Response string: " << response_string.substr(0, 1000) << (response_string.length() > 1000 ? "..." : "") << std::endl;
            // Return empty JSON object instead of throwing
            return json::object();
        } catch (const std::exception& e) {
            std::cerr << "Error processing response: " << e.what() << std::endl;
            std::cerr << "Response string: " << response_string.substr(0, 1000) << (response_string.length() > 1000 ? "..." : "") << std::endl;
            // Return empty JSON object instead of throwing
            return json::object();
        }
        
        // Check HTTP status code
        if (http_code >= 400) {
            std::cerr << "HTTP request failed with code " << http_code << ": " << response_string << std::endl;
            // Return the error response JSON instead of throwing
            return response_json;
        }
        
        return response_json;
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

    std::chrono::system_clock::time_point calculateNextFundingTime() {
        // Binance funding times occur at 00:00, 08:00, and 16:00 UTC
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
std::shared_ptr<ExchangeInterface> createBinanceExchange(const ExchangeConfig& config) {
    return std::make_shared<BinanceExchange>(config);
}

} // namespace funding 