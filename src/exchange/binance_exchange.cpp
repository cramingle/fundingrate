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
            bool is_futures = false;
            
            // Different endpoints for different market types
            switch (type) {
                case MarketType::SPOT:
                    endpoint = "/api/v3/exchangeInfo";
                    break;
                case MarketType::PERPETUAL:
                    endpoint = "/fapi/v1/exchangeInfo";
                    is_futures = true;
                    break;
                case MarketType::MARGIN:
                    endpoint = "/sapi/v1/margin/allPairs";
                    break;
                default:
                    throw std::runtime_error("Unsupported market type");
            }
            
            // Make API call with futures_api flag if needed
            json response = makeApiCall(endpoint, "", false, "GET", is_futures);
            
            // Check if the response is valid
            if (!response.is_object()) {
                throw std::runtime_error("Invalid response format: not an object");
            }
            
            // For spot and perpetual markets
            if (type == MarketType::SPOT || type == MarketType::PERPETUAL) {
                // Check if the response contains the symbols field
                if (!response.contains("symbols")) {
                    throw std::runtime_error("Invalid response format: missing symbols field");
                }
                
                // Check if symbols is an array
                if (!response.at("symbols").is_array()) {
                    throw std::runtime_error("Invalid response format: symbols is not an array");
                }
                
                std::cout << "Processing " << response.at("symbols").size() << " symbols from Binance" << std::endl;
                
                // Process each symbol
                for (const auto& symbol : response.at("symbols")) {
                    try {
                        Instrument instrument;
                        
                        // Skip if symbol doesn't contain required fields
                        if (!symbol.contains("symbol") || !symbol.contains("status")) {
                            continue;
                        }
                        
                        // Skip if symbol is not trading
                        if (symbol.at("status") != "TRADING") {
                            continue;
                        }
                        
                        instrument.symbol = symbol.at("symbol");
                        instrument.market_type = type;
                        
                        // Extract base and quote currencies
                        if (symbol.contains("baseAsset") && symbol.contains("quoteAsset")) {
                            instrument.base_currency = symbol.at("baseAsset");
                            instrument.quote_currency = symbol.at("quoteAsset");
                        }
                        
                        // Extract price precision
                        if (symbol.contains("pricePrecision")) {
                            instrument.price_precision = symbol.at("pricePrecision");
                        } else if (symbol.contains("quotePrecision")) {
                            instrument.price_precision = symbol.at("quotePrecision");
                        }
                        
                        // Extract quantity precision
                        if (symbol.contains("baseAssetPrecision")) {
                            instrument.qty_precision = symbol.at("baseAssetPrecision");
                        }
                        
                        // Extract minimum notional value and set as min_order_size
                        if (symbol.contains("filters") && symbol.at("filters").is_array()) {
                            for (const auto& filter : symbol.at("filters")) {
                                if (filter.contains("filterType") && filter.at("filterType") == "NOTIONAL") {
                                    if (filter.contains("minNotional")) {
                                        instrument.min_order_size = std::stod(filter.at("minNotional").get<std::string>());
                                        instrument.min_qty = instrument.min_order_size; // Set alias field
                                    }
                                }
                            }
                        }
                        
                        // Add the instrument to the list
                        instruments.push_back(instrument);
                    } catch (const std::exception& e) {
                        // Log error but continue processing other symbols
                        std::cerr << "Error processing symbol: " << e.what() << std::endl;
                        continue;
                    }
                }
            }
            // For margin markets
            else if (type == MarketType::MARGIN) {
                // Process margin pairs
                for (const auto& pair : response) {
                    try {
                        Instrument instrument;
                        
                        // Skip if pair doesn't contain required fields
                        if (!pair.contains("symbol") || !pair.contains("base") || !pair.contains("quote")) {
                            continue;
                        }
                        
                        instrument.symbol = pair.at("symbol");
                        instrument.market_type = MarketType::MARGIN;
                        instrument.base_currency = pair.at("base");
                        instrument.quote_currency = pair.at("quote");
                        
                        // Add the instrument to the list
                        instruments.push_back(instrument);
                    } catch (const std::exception& e) {
                        // Log error but continue processing other pairs
                        std::cerr << "Error processing margin pair: " << e.what() << std::endl;
                        continue;
                    }
                }
            }
            
            std::cout << "Found " << instruments.size() << " instruments for market type " 
                      << static_cast<int>(type) << " on Binance" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching instruments from Binance: " << e.what() << std::endl;
        }
        
        return instruments;
    }
    
    double getPrice(const std::string& symbol) override {
        double price = 0.0;
        
        // Check cache first
        auto cache_it = price_cache_.find(symbol);
        if (cache_it != price_cache_.end()) {
            auto now = std::chrono::system_clock::now();
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - cache_it->second.timestamp).count();
            
            // Use cached price if it's less than 5 seconds old
            if (age < 5) {
                return cache_it->second.price;
            }
        }
        
        try {
            std::string endpoint;
            
            // Different endpoints for different market types
            if (symbol.find("USDT") != std::string::npos || 
                symbol.find("BUSD") != std::string::npos || 
                symbol.find("USDC") != std::string::npos) {
                // Try futures API first for common perpetual contracts
                std::string original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
                
                endpoint = "/fapi/v1/ticker/price?symbol=" + symbol;
                
                try {
                    json response = makeApiCall(endpoint);
                    
                    // Check if the response is valid
                    if (!response.is_object()) {
                        throw std::runtime_error("Invalid response format: not an object");
                    }
                    
                    // Check if the response contains the price field
                    if (!response.contains("price")) {
                        throw std::runtime_error("Invalid response format: missing price field");
                    }
                    
                    // Parse price with type checking
                    if (response.at("price").is_string()) {
                        price = std::stod(response.at("price").get<std::string>());
                    } else if (response.at("price").is_number()) {
                        price = response.at("price").get<double>();
                    } else {
                        throw std::runtime_error("Invalid price format: neither string nor number");
                    }
                    
                    // Restore original base URL
                    base_url_ = original_base_url;
                    
                    // Update cache
                    PriceCacheEntry entry;
                    entry.price = price;
                    entry.timestamp = std::chrono::system_clock::now();
                    price_cache_[symbol] = entry;
                    
                    return price;
                } catch (const std::exception& e) {
                    // Restore original base URL before trying spot API
                    base_url_ = original_base_url;
                    
                    // Fall through to try spot API
                    std::cerr << "Error fetching futures price for " << symbol << ": " << e.what() 
                              << ". Trying spot API..." << std::endl;
                }
            }
            
            // Try spot API
            endpoint = "/api/v3/ticker/price?symbol=" + symbol;
            json response = makeApiCall(endpoint);
            
            // Check if the response is valid
            if (!response.is_object()) {
                throw std::runtime_error("Invalid response format: not an object");
            }
            
            // Check if the response contains the price field
            if (!response.contains("price")) {
                throw std::runtime_error("Invalid response format: missing price field");
            }
            
            // Parse price with type checking
            if (response.at("price").is_string()) {
                price = std::stod(response.at("price").get<std::string>());
            } else if (response.at("price").is_number()) {
                price = response.at("price").get<double>();
            } else {
                throw std::runtime_error("Invalid price format: neither string nor number");
            }
            
            // Update cache
            PriceCacheEntry entry;
            entry.price = price;
            entry.timestamp = std::chrono::system_clock::now();
            price_cache_[symbol] = entry;
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching price for " << symbol << ": " << e.what() << std::endl;
        }
        
        return price;
    }
    
    OrderBook getOrderBook(const std::string& symbol, int depth) override {
        OrderBook book;
        book.symbol = symbol;
        
        try {
            // Determine if this is a futures symbol
            bool is_futures = symbol.find("USDT") != std::string::npos ||
                              symbol.find("BUSD") != std::string::npos ||
                              symbol.find("USDC") != std::string::npos;
            
            std::string original_base_url;
            if (is_futures) {
                original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
            }
            
            // Set endpoint and parameters
            std::string endpoint = is_futures ? "/fapi/v1/depth" : "/api/v3/depth";
            std::string params = "symbol=" + symbol + "&limit=" + std::to_string(depth);
            
            // Make API call
            json response;
            try {
                response = makeApiCall(endpoint, params);
            } catch (const std::exception& e) {
                // Restore original base URL if needed
                if (is_futures) {
                    base_url_ = original_base_url;
                }
                
                // If we failed with futures API, try with spot API
                if (is_futures) {
                    is_futures = false;
                    endpoint = "/api/v3/depth";
                    try {
                        response = makeApiCall(endpoint, params);
                    } catch (const std::exception& e) {
                        std::cerr << "Error fetching order book for " << symbol << " with spot API: " << e.what() << std::endl;
                        throw;
                    }
                } else {
                    throw;
                }
            }
            
            // Restore original base URL if needed
            if (is_futures) {
                base_url_ = original_base_url;
            }
            
            // Process bids
            if (response.contains("bids") && response.at("bids").is_array()) {
                for (const auto& bid : response.at("bids")) {
                    if (bid.is_array() && bid.size() >= 2) {
                        PriceLevel level;
                        level.price = std::stod(bid.at(0).get<std::string>());
                        level.amount = std::stod(bid.at(1).get<std::string>());
                        book.bids.push_back(level);
                    }
                }
            }
            
            // Process asks
            if (response.contains("asks") && response.at("asks").is_array()) {
                for (const auto& ask : response.at("asks")) {
                    if (ask.is_array() && ask.size() >= 2) {
                        PriceLevel level;
                        level.price = std::stod(ask.at(0).get<std::string>());
                        level.amount = std::stod(ask.at(1).get<std::string>());
                        book.asks.push_back(level);
                    }
                }
            }
            
            // Set timestamp
            if (response.contains("lastUpdateId")) {
                book.timestamp = std::chrono::system_clock::now();
            }
            
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
                          << ": Code " << response.at("code").get<int>() 
                          << ", Message: " << response.at("msg").get<std::string>() << std::endl;
                
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
                    if (response.at("lastFundingRate").is_string()) {
                        funding.rate = std::stod(response.at("lastFundingRate").get<std::string>());
                    } else if (response.at("lastFundingRate").is_number()) {
                        funding.rate = response.at("lastFundingRate").get<double>();
                    } else {
                        std::cerr << "lastFundingRate is neither a string nor a number" << std::endl;
                        funding.rate = 0.0;
                    }
                    
                    std::cout << "Funding rate: " << funding.rate << std::endl;
                    
                    // Get next funding time
                    if (response.contains("nextFundingTime")) {
                        try {
                            int64_t next_funding_time = 0;
                            
                            if (response.at("nextFundingTime").is_string()) {
                                next_funding_time = std::stoll(response.at("nextFundingTime").get<std::string>());
                            } else if (response.at("nextFundingTime").is_number()) {
                                next_funding_time = response.at("nextFundingTime").get<int64_t>();
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
                            if (response.at("predictedFundingRate").is_string()) {
                                funding.predicted_rate = std::stod(response.at("predictedFundingRate").get<std::string>());
                            } else if (response.at("predictedFundingRate").is_number()) {
                                funding.predicted_rate = response.at("predictedFundingRate").get<double>();
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
            // Get timestamp for signature
            std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            // Create query string
            std::string query_string = "timestamp=" + timestamp;
            
            // Generate signature
            std::string signature = generateSignature(query_string);
            
            // Make API call
            json response = makeApiCall("/api/v3/account", query_string + "&signature=" + signature, true);
            
            // Process balances
            if (response.contains("balances") && response.at("balances").is_array()) {
                for (const auto& asset : response.at("balances")) {
                    if (asset.contains("asset") && asset.contains("free") && asset.contains("locked")) {
                        std::string currency = asset.at("asset");
                        double free_amount = std::stod(asset.at("free").get<std::string>());
                        double locked_amount = std::stod(asset.at("locked").get<std::string>());
                        
                        // Only add non-zero balances
                        if (free_amount > 0 || locked_amount > 0) {
                            balance.total[currency] = free_amount + locked_amount;
                            balance.available[currency] = free_amount;
                            balance.locked[currency] = locked_amount;
                        }
                    }
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching account balance: " << e.what() << std::endl;
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
            // Convert symbol to uppercase for consistency
            std::string upper_symbol = order.symbol;
            std::transform(upper_symbol.begin(), upper_symbol.end(), upper_symbol.begin(), ::toupper);
            
            // Determine if this is a futures symbol
            bool is_futures = upper_symbol.find("PERP") != std::string::npos ||
                              upper_symbol.find("USDT") != std::string::npos ||
                              upper_symbol.find("BUSD") != std::string::npos ||
                              upper_symbol.find("USDC") != std::string::npos;
            
            // Save original base URL if using futures API
            std::string original_base_url;
            if (is_futures) {
                original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
            }
            
            // Get timestamp for signature
            std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            // Build query string
            std::string side = (order.side == OrderSide::BUY) ? "BUY" : "SELL";
            std::string type = (order.type == OrderType::MARKET) ? "MARKET" : "LIMIT";
            
            std::string query_string = "symbol=" + upper_symbol +
                                      "&side=" + side +
                                      "&type=" + type;
            
            // Add quantity
            query_string += "&quantity=" + std::to_string(order.amount);
            
            // Add price for limit orders
            if (order.type == OrderType::LIMIT) {
                query_string += "&price=" + std::to_string(order.price);
                query_string += "&timeInForce=GTC";  // Good Till Cancelled
            }
            
            // Add timestamp
            query_string += "&timestamp=" + timestamp;
            
            // Generate signature
            std::string signature = generateSignature(query_string);
            query_string += "&signature=" + signature;
            
            // Set endpoint based on market type
            std::string endpoint = is_futures ? "/fapi/v1/order" : "/api/v3/order";
            
            // Make API call
            json response = makeApiCall(endpoint, query_string, true, "POST", is_futures);
            
            // Restore original base URL if needed
            if (is_futures) {
                base_url_ = original_base_url;
            }
            
            // Check if the response contains the orderId field
            if (!response.contains("orderId")) {
                throw std::runtime_error("Order placement failed: missing orderId in response");
            }
            
            // Return the order ID
            return std::to_string(response.at("orderId").get<int64_t>());
            
        } catch (const std::exception& e) {
            std::cerr << "Error placing order on Binance: " << e.what() << std::endl;
            throw;
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
            // Get timestamp for signature
            std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            
            // Build query string
            std::string query_string = "orderId=" + order_id + "&timestamp=" + timestamp;
            
            // Generate signature
            std::string signature = generateSignature(query_string);
            query_string += "&signature=" + signature;
            
            // Make API call
            json response = makeApiCall("/api/v3/order", query_string, true);
            
            // Check if the response contains the status field
            if (!response.contains("status")) {
                throw std::runtime_error("Order status check failed: missing status in response");
            }
            
            // Extract status string
            std::string status = response.at("status").get<std::string>();
            
            // Convert string status to enum
            if (status == "NEW" || status == "PARTIALLY_FILLED") {
                return OrderStatus::PARTIALLY_FILLED;
            } else if (status == "FILLED") {
                return OrderStatus::FILLED;
            } else if (status == "CANCELED" || status == "EXPIRED" || status == "REJECTED") {
                return OrderStatus::CANCELED;
            } else {
                return OrderStatus::REJECTED;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error checking order status on Binance: " << e.what() << std::endl;
            return OrderStatus::REJECTED;
        }
    }
    
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit) override {
        std::vector<Trade> trades;
        
        try {
            // Convert symbol to uppercase for consistency
            std::string upper_symbol = symbol;
            std::transform(upper_symbol.begin(), upper_symbol.end(), upper_symbol.begin(), ::toupper);
            
            // Determine if this is a futures symbol
            bool is_futures = upper_symbol.find("PERP") != std::string::npos ||
                              upper_symbol.find("USDT") != std::string::npos ||
                              upper_symbol.find("BUSD") != std::string::npos ||
                              upper_symbol.find("USDC") != std::string::npos;
            
            // Save original base URL if using futures API
            std::string original_base_url;
            if (is_futures) {
                original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
            }
            
            // Set endpoint and parameters
            std::string endpoint = is_futures ? "/fapi/v1/trades" : "/api/v3/trades";
            std::string query_string = "symbol=" + upper_symbol + "&limit=" + std::to_string(limit);
            
            // Make API call
            json response = makeApiCall(endpoint, query_string);
            
            // Restore original base URL if needed
            if (is_futures) {
                base_url_ = original_base_url;
            }
            
            // Check if the response is an array
            if (!response.is_array()) {
                throw std::runtime_error("Invalid response format: not an array");
            }
            
            // Process each trade
            for (const auto& trade_data : response) {
                try {
                    // Check if the trade data contains all required fields
                    if (!trade_data.contains("id") || !trade_data.contains("price") || 
                        !trade_data.contains("qty") || !trade_data.contains("time") || 
                        !trade_data.contains("isBuyerMaker")) {
                        continue;
                    }
                    
                    Trade trade;
                    trade.symbol = upper_symbol;
                    trade.price = std::stod(trade_data.at("price").get<std::string>());
                    trade.amount = std::stod(trade_data.at("qty").get<std::string>());
                    trade.side = trade_data.at("isBuyerMaker").get<bool>() ? "sell" : "buy";
                    trade.trade_id = std::to_string(trade_data.at("id").get<int64_t>());
                    
                    // Convert timestamp to time_point
                    int64_t timestamp_ms = trade_data.at("time").get<int64_t>();
                    trade.timestamp = std::chrono::system_clock::time_point(
                        std::chrono::milliseconds(timestamp_ms));
                    
                    trades.push_back(trade);
                } catch (const std::exception& e) {
                    // Log error but continue processing other trades
                    std::cerr << "Error processing trade: " << e.what() << std::endl;
                    continue;
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching recent trades from Binance: " << e.what() << std::endl;
        }
        
        return trades;
    }
    
    std::vector<Candle> getCandles(const std::string& symbol, 
                                 const std::string& interval,
                                 const std::chrono::system_clock::time_point& start,
                                 const std::chrono::system_clock::time_point& end) override {
        std::vector<Candle> candles;
        
        try {
            // Convert symbol to uppercase for consistency
            std::string upper_symbol = symbol;
            std::transform(upper_symbol.begin(), upper_symbol.end(), upper_symbol.begin(), ::toupper);
            
            // Determine if this is a futures symbol
            bool is_futures = upper_symbol.find("PERP") != std::string::npos ||
                              upper_symbol.find("USDT") != std::string::npos ||
                              upper_symbol.find("BUSD") != std::string::npos ||
                              upper_symbol.find("USDC") != std::string::npos;
            
            // Save original base URL if using futures API
            std::string original_base_url;
            if (is_futures) {
                original_base_url = base_url_;
                base_url_ = "https://fapi.binance.com";
            }
            
            // Set endpoint and parameters
            std::string endpoint = is_futures ? "/fapi/v1/klines" : "/api/v3/klines";
            
            // Convert time points to timestamps in milliseconds
            int64_t start_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                start.time_since_epoch()).count();
            int64_t end_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                end.time_since_epoch()).count();
            
            std::string query_string = "symbol=" + upper_symbol + 
                                     "&interval=" + interval + 
                                     "&startTime=" + std::to_string(start_ts) + 
                                     "&endTime=" + std::to_string(end_ts) + 
                                     "&limit=1000"; // Maximum allowed by Binance
            
            // Make API call
            json response = makeApiCall(endpoint, query_string);
            
            // Restore original base URL if needed
            if (is_futures) {
                base_url_ = original_base_url;
            }
            
            // Check if the response is an array
            if (!response.is_array()) {
                throw std::runtime_error("Invalid response format: not an array");
            }
            
            // Process each candle
            for (const auto& candle_data : response) {
                try {
                    // Check if the candle data has the expected format
                    if (!candle_data.is_array() || candle_data.size() < 9) {
                        continue;
                    }
                    
                    Candle candle;
                    candle.symbol = upper_symbol;
                    
                    // Binance returns [open_time, open, high, low, close, volume, close_time, ...]
                    int64_t open_ts = candle_data.at(0).get<int64_t>();
                    int64_t close_ts = candle_data.at(6).get<int64_t>();
                    
                    candle.open_time = std::chrono::system_clock::from_time_t(open_ts / 1000);
                    candle.close_time = std::chrono::system_clock::from_time_t(close_ts / 1000);
                    
                    candle.open = std::stod(candle_data.at(1).get<std::string>());
                    candle.high = std::stod(candle_data.at(2).get<std::string>());
                    candle.low = std::stod(candle_data.at(3).get<std::string>());
                    candle.close = std::stod(candle_data.at(4).get<std::string>());
                    candle.volume = std::stod(candle_data.at(5).get<std::string>());
                    candle.trades = candle_data.at(8).get<int>();
                    
                    candles.push_back(candle);
                } catch (const std::exception& e) {
                    // Log error but continue processing other candles
                    std::cerr << "Error processing candle: " << e.what() << std::endl;
                    continue;
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error fetching candles from Binance: " << e.what() << std::endl;
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