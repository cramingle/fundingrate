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

// Bitget API implementation
class BitgetExchange : public ExchangeInterface {
public:
    BitgetExchange(const ExchangeConfig& config) : 
        api_key_(config.getApiKey()),
        api_secret_(config.getApiSecret()),
        passphrase_(config.getParam("passphrase")),  // Bitget requires a passphrase
        base_url_("https://api.bitget.com"),
        use_testnet_(config.getUseTestnet()),
        last_fee_update_(std::chrono::system_clock::now() - std::chrono::hours(25)) { // Force initial fee update
        
        if (use_testnet_) {
            base_url_ = "https://bitgetlimited.github.io/apidemo"; // Demo API URL
        }
        
        // Initialize CURL
        curl_global_init(CURL_GLOBAL_ALL);
        
        // Connect and fetch initial fee structure
        reconnect();
        updateFeeStructure();
    }
    
    ~BitgetExchange() {
        curl_global_cleanup();
    }
    
    // Exchange information
    std::string getName() const override {
        return "Bitget";
    }
    
    std::string getBaseUrl() const override {
        return base_url_;
    }
    
    // Market data
    std::vector<Instrument> getAvailableInstruments(MarketType type) override {
        std::vector<Instrument> instruments;
        std::string endpoint;
        
        // Bitget has different endpoints for different markets
        switch (type) {
            case MarketType::SPOT:
                endpoint = "/api/spot/v1/public/products";
                break;
            case MarketType::PERPETUAL:
                endpoint = "/api/mix/v1/market/contracts?productType=umcbl";  // USDT-margined linear contracts
                break;
            case MarketType::MARGIN:
                endpoint = "/api/margin/v1/public/symbols"; // Margin trading
                break;
            default:
                throw std::runtime_error("Unsupported market type");
        }
        
        json response = makeApiCall(endpoint, "", false);
        
        try {
            if (response.contains("code") && response["code"] == "00000" && response.contains("data")) {
                auto& data = response["data"];
                
                if (type == MarketType::SPOT) {
                    // Process spot instruments
                    for (const auto& item : data) {
                        // Skip if not active
                        if (item.contains("status") && item["status"] != "online") {
                            continue;
                        }
                        
                        Instrument inst;
                        inst.symbol = item["symbolName"];
                        inst.base_currency = item["baseCoin"];
                        inst.quote_currency = item["quoteCoin"];
                        inst.market_type = type;
                        
                        // Get min/max quantities and precision
                        inst.min_order_size = std::stod(item["minTradeAmount"].get<std::string>());
                        inst.min_qty = inst.min_order_size; // Alias
                        
                        // We'll use a high default max qty
                        inst.max_qty = 999999.0;
                        if (item.contains("maxTradeAmount")) {
                            inst.max_qty = std::stod(item["maxTradeAmount"].get<std::string>());
                        }
                        
                        // Price precision
                        inst.price_precision = std::stoi(item["priceScale"].get<std::string>());
                        inst.tick_size = 1.0 / std::pow(10, inst.price_precision);
                        
                        // Quantity precision
                        inst.qty_precision = std::stoi(item["quantityScale"].get<std::string>());
                        
                        instruments.push_back(inst);
                    }
                } else if (type == MarketType::PERPETUAL) {
                    // Process perpetual instruments
                    for (const auto& item : data) {
                        // Skip if not active
                        if (item.contains("status") && item["status"] != "normal") {
                            continue;
                        }
                        
                        Instrument inst;
                        inst.symbol = item["symbol"];
                        
                        // Extract base and quote from symbol (e.g., BTCUSDT_UMCBL -> BTC and USDT)
                        std::string symbol = item["symbol"];
                        size_t pos = symbol.find("USDT");
                        if (pos != std::string::npos) {
                            inst.base_currency = symbol.substr(0, pos);
                            inst.quote_currency = "USDT";
                        } else {
                            // Default fallback for unusual symbols
                            inst.base_currency = symbol;
                            inst.quote_currency = "USDT";
                        }
                        
                        inst.market_type = type;
                        
                        // Get min/max quantities and precision
                        inst.min_order_size = std::stod(item["minTradeNum"].get<std::string>());
                        inst.min_qty = inst.min_order_size; // Alias
                        
                        // Use a high default for max qty
                        inst.max_qty = 999999.0;
                        if (item.contains("maxTradeAmount")) {
                            inst.max_qty = std::stod(item["maxTradeAmount"].get<std::string>());
                        }
                        
                        // Price precision
                        inst.price_precision = std::stoi(item["priceEndStep"].get<std::string>());
                        inst.tick_size = std::stod(item["priceEndStep"].get<std::string>());
                        
                        // Quantity precision (default to 8 if not specified)
                        inst.qty_precision = 8;
                        if (item.contains("volumePlace")) {
                            inst.qty_precision = std::stoi(item["volumePlace"].get<std::string>());
                        }
                        
                        instruments.push_back(inst);
                    }
                } else if (type == MarketType::MARGIN) {
                    // Process margin instruments
                    for (const auto& item : data) {
                        Instrument inst;
                        inst.symbol = item["symbol"];
                        
                        // Extract base and quote from symbol
                        size_t pos = inst.symbol.find("_");
                        if (pos != std::string::npos) {
                            inst.base_currency = inst.symbol.substr(0, pos);
                            inst.quote_currency = inst.symbol.substr(pos + 1);
                        } else {
                            // Fallback
                            inst.base_currency = inst.symbol;
                            inst.quote_currency = "USDT";
                        }
                        
                        inst.market_type = type;
                        
                        // Default values - would need additional API calls for exact limits
                        inst.min_order_size = 0.0001;
                        inst.min_qty = 0.0001;
                        inst.max_qty = 999999.0;
                        inst.price_precision = 8;
                        inst.tick_size = 0.00000001;
                        inst.qty_precision = 8;
                        
                        instruments.push_back(inst);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error parsing Bitget instruments: " << e.what() << std::endl;
        }
        
        return instruments;
    }
    
    double getPrice(const std::string& symbol) override {
        // Determine if this is a spot or contract symbol
        bool is_contract = symbol.find("_UMCBL") != std::string::npos;
        
        std::string endpoint = is_contract ? 
            "/api/mix/v1/market/ticker?symbol=" + symbol :
            "/api/spot/v1/market/ticker?symbol=" + symbol;
        
        json response = makeApiCall(endpoint, "", false);
        
        if (response["code"] == "00000" && response.contains("data")) {
            return std::stod(response["data"]["last"].get<std::string>());
        }
        
        throw std::runtime_error("Failed to get price for " + symbol);
    }
    
    OrderBook getOrderBook(const std::string& symbol, int depth) override {
        OrderBook book;
        book.symbol = symbol;
        book.timestamp = std::chrono::system_clock::now();
        
        try {
            // Determine if this is a spot or contract symbol
            bool is_contract = symbol.find("_UMCBL") != std::string::npos;
            
            std::string endpoint = is_contract ?
                "/api/mix/v1/market/depth?symbol=" + symbol + "&limit=" + std::to_string(depth) :
                "/api/spot/v1/market/depth?symbol=" + symbol + "&limit=" + std::to_string(depth);
            
            json response = makeApiCall(endpoint, "", false);
            
            if (response["code"] == "00000" && response.contains("data")) {
                // Parse asks
                if (response["data"].contains("asks")) {
                    for (const auto& ask : response["data"]["asks"]) {
                        if (ask.size() >= 2) {
                            OrderBookLevel level;
                            level.price = std::stod(ask[0].get<std::string>());
                            level.amount = std::stod(ask[1].get<std::string>());
                            book.asks.push_back(level);
                        }
                    }
                }
                
                // Parse bids
                if (response["data"].contains("bids")) {
                    for (const auto& bid : response["data"]["bids"]) {
                        if (bid.size() >= 2) {
                            OrderBookLevel level;
                            level.price = std::stod(bid[0].get<std::string>());
                            level.amount = std::stod(bid[1].get<std::string>());
                            book.bids.push_back(level);
                        }
                    }
                }
                
                // Bitget timestamps may be in the response
                if (response["data"].contains("ts")) {
                    auto ts = std::stoll(response["data"]["ts"].get<std::string>());
                    book.timestamp = std::chrono::system_clock::from_time_t(ts / 1000);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching order book from Bitget: " << e.what() << std::endl;
        }
        
        return book;
    }
    
    FundingRate getFundingRate(const std::string& symbol) override {
        FundingRate funding;
        funding.symbol = symbol;
        
        try {
            // Funding rate is only available for contract trading
            if (symbol.find("_UMCBL") == std::string::npos) {
                throw std::runtime_error("Funding rate only available for UMCBL contracts");
            }
            
            std::string endpoint = "/api/mix/v1/market/funding-time?symbol=" + symbol;
            json response = makeApiCall(endpoint, "", false);
            
            if (response["code"] == "00000" && response.contains("data")) {
                auto& data = response["data"];
                
                // Parse current funding rate
                if (data.contains("fundingRate")) {
                    funding.rate = std::stod(data["fundingRate"].get<std::string>());
                }
                
                // Parse next funding time
                if (data.contains("nextFundingTime")) {
                    std::string next_time_str = data["nextFundingTime"].get<std::string>();
                    int64_t next_time = std::stoll(next_time_str);
                    funding.next_payment = std::chrono::system_clock::from_time_t(next_time / 1000);
                }
                
                // Parse predicted rate if available
                if (data.contains("predictedRate")) {
                    std::string predicted_rate_str = data["predictedRate"].get<std::string>();
                    funding.predicted_rate = std::stod(predicted_rate_str);
                } else {
                    funding.predicted_rate = funding.rate; // Use current rate as prediction if not available
                }
                
                // Bitget has 8-hour funding intervals
                funding.payment_interval = std::chrono::hours(8);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching funding rate from Bitget: " << e.what() << std::endl;
            // Set default values for funding rate
            funding.rate = 0.0;
            funding.predicted_rate = 0.0;
            funding.payment_interval = std::chrono::hours(8);
            funding.next_payment = std::chrono::system_clock::now() + std::chrono::hours(8);
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
        
        // Determine market type based on symbol
        bool is_contract = symbol.find("_UMCBL") != std::string::npos;
        if (is_contract) {
            return is_maker ? fee_structure_.perp_maker_fee : fee_structure_.perp_taker_fee;
        } else if (symbol.find("_MARGIN") != std::string::npos) {
            return is_maker ? fee_structure_.margin_maker_fee : fee_structure_.margin_taker_fee;
        } else {
            // Default to spot fees
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
        AccountBalance balance;
        
        try {
            // First get spot account balance
            std::string spot_endpoint = "/api/spot/v1/account/assets";
            json spot_response = makeApiCall(spot_endpoint, "", true);
            
            if (spot_response["code"] == "00000" && spot_response.contains("data")) {
                for (const auto& asset : spot_response["data"]) {
                    std::string currency = asset["coinName"];
                    double total = std::stod(asset["total"].get<std::string>());
                    double available = std::stod(asset["available"].get<std::string>());
                    double locked = std::stod(asset["locked"].get<std::string>());
                    
                    balance.total[currency] = total;
                    balance.available[currency] = available;
                    balance.locked[currency] = locked;
                }
            }
            
            // Then get futures account balance
            std::string futures_endpoint = "/api/mix/v1/account/accounts?productType=umcbl";
            json futures_response = makeApiCall(futures_endpoint, "", true);
            
            if (futures_response["code"] == "00000" && futures_response.contains("data")) {
                for (const auto& asset : futures_response["data"]) {
                    std::string currency = asset["marginCoin"];
                    double equity = std::stod(asset["equity"].get<std::string>());
                    double available = std::stod(asset["available"].get<std::string>());
                    double locked = equity - available;
                    
                    // Add to existing balance or create new entry
                    if (balance.total.find(currency) != balance.total.end()) {
                        balance.total[currency] += equity;
                        balance.available[currency] += available;
                        balance.locked[currency] += locked;
                    } else {
                        balance.total[currency] = equity;
                        balance.available[currency] = available;
                        balance.locked[currency] = locked;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Bitget account balance: " << e.what() << std::endl;
        }
        
        return balance;
    }
    
    std::vector<Position> getOpenPositions() override {
        std::vector<Position> positions;
        
        try {
            // Get open positions for USDT-margined contracts
            std::string endpoint = "/api/mix/v1/position/allPosition?productType=umcbl";
            json response = makeApiCall(endpoint, "", true);
            
            if (response["code"] == "00000" && response.contains("data")) {
                for (const auto& pos_data : response["data"]) {
                    // Skip if position size is zero
                    double size = std::stod(pos_data["total"].get<std::string>());
                    if (size <= 0) {
                        continue;
                    }
                    
                    Position position;
                    position.symbol = pos_data["symbol"];
                    std::string side = pos_data["holdSide"]; // "long" or "short"
                    
                    // Set position size (positive for long, negative for short)
                    position.size = (side == "long") ? size : -size;
                    
                    position.entry_price = std::stod(pos_data["averageOpenPrice"].get<std::string>());
                    
                    // Get liquidation price (may not be available directly)
                    if (pos_data.contains("liquidationPrice")) {
                        position.liquidation_price = std::stod(pos_data["liquidationPrice"].get<std::string>());
                    } else {
                        // Use a fallback value
                        position.liquidation_price = 0.0;
                    }
                    
                    position.unrealized_pnl = std::stod(pos_data["unrealizedPL"].get<std::string>());
                    position.leverage = std::stod(pos_data["leverage"].get<std::string>());
                    
                    positions.push_back(position);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching Bitget positions: " << e.what() << std::endl;
        }
        
        return positions;
    }
    
    // Trading operations
    std::string placeOrder(const Order& order) override {
        try {
            // Determine if this is a spot or contract order
            bool is_contract = order.symbol.find("_UMCBL") != std::string::npos;
            
            // Prepare the order parameters
            json params;
            params["symbol"] = order.symbol;
            
            // Convert order side
            std::string side_str = (order.side == OrderSide::BUY) ? "buy" : "sell";
            
            if (is_contract) {
                // For contract trading
                params["marginCoin"] = order.symbol.substr(0, order.symbol.find("_"));
                params["side"] = side_str;
                
                // Set order type
                if (order.type == OrderType::LIMIT) {
                    params["orderType"] = "limit";
                    params["price"] = std::to_string(order.price);
                } else {
                    params["orderType"] = "market";
                }
                
                // Set quantity
                params["size"] = std::to_string(order.amount);
                
                // Set time in force
                params["timeInForceValue"] = "normal"; // GTC - Good Till Cancel
                
                // API endpoint for contract trading
                std::string endpoint = "/api/mix/v1/order/placeOrder";
                json response = makeApiCall(endpoint, params.dump(), true, "POST");
                
                if (response["code"] == "00000" && response.contains("data")) {
                    std::string order_id = response["data"]["orderId"].get<std::string>();
                    // Store order type
                    order_types_[order_id] = true; // true for contract/futures
                    return order_id;
                } else {
                    throw std::runtime_error("Failed to place contract order: " + response.dump());
                }
            } else {
                // For spot trading
                params["side"] = side_str;
                
                // Set order type and price
                if (order.type == OrderType::LIMIT) {
                    params["orderType"] = "limit";
                    params["price"] = std::to_string(order.price);
                } else {
                    params["orderType"] = "market";
                }
                
                // Set quantity
                params["quantity"] = std::to_string(order.amount);
                
                // Set force and client order ID if needed
                params["force"] = "normal"; // GTC - Good Till Cancel
                
                // API endpoint for spot trading
                std::string endpoint = "/api/spot/v1/trade/orders";
                json response = makeApiCall(endpoint, params.dump(), true, "POST");
                
                if (response["code"] == "00000" && response.contains("data")) {
                    std::string order_id = response["data"]["orderId"].get<std::string>();
                    // Store order type
                    order_types_[order_id] = false; // false for spot
                    return order_id;
                } else {
                    throw std::runtime_error("Failed to place spot order: " + response.dump());
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error placing order on Bitget: " << e.what() << std::endl;
            return "";
        }
    }
    
    bool cancelOrder(const std::string& order_id) override {
        try {
            // Check if we have stored the order type
            auto it = order_types_.find(order_id);
            bool is_contract = false;
            bool order_type_known = (it != order_types_.end());
            
            if (order_type_known) {
                is_contract = it->second;
            }
            
            bool success = false;
            json response;
            
            // If we know the order type, try that endpoint first
            if (order_type_known) {
                if (is_contract) {
                    // Try futures endpoint
                    std::string futures_endpoint = "/api/mix/v1/order/cancel-order";
                    json futures_req = {
                        {"orderId", order_id},
                        {"marginCoin", "USDT"}, // Default to USDT, ideally we'd store the actual marginCoin
                        {"symbol", ""} // Without symbol, will need to be determined from order ID
                    };
                    
                    response = makeApiCall(futures_endpoint, futures_req.dump(), true, "POST");
                    if (response["code"] == "00000") {
                        success = true;
                    }
                } else {
                    // Try spot endpoint
                    std::string spot_endpoint = "/api/spot/v1/trade/cancel-order";
                    json spot_req = {
                        {"orderId", order_id}
                    };
                    
                    response = makeApiCall(spot_endpoint, spot_req.dump(), true, "POST");
                    if (response["code"] == "00000") {
                        success = true;
                    }
                }
            } else {
                // If order type is unknown, try both endpoints as before
                // Try to cancel as spot order first
                try {
                    std::string spot_endpoint = "/api/spot/v1/trade/cancel-order";
                    json spot_req = {
                        {"orderId", order_id}
                    };
                    
                    response = makeApiCall(spot_endpoint, spot_req.dump(), true, "POST");
                    if (response["code"] == "00000") {
                        success = true;
                        // Store for future reference
                        order_types_[order_id] = false;
                    }
                } catch (...) {
                    // If spot cancel fails, try futures
                }
                
                // If spot cancel failed, try futures
                if (!success) {
                    std::string futures_endpoint = "/api/mix/v1/order/cancel-order";
                    json futures_req = {
                        {"orderId", order_id},
                        {"marginCoin", "USDT"},
                        {"symbol", ""} // Without symbol, will need to be determined from order ID
                    };
                    
                    response = makeApiCall(futures_endpoint, futures_req.dump(), true, "POST");
                    if (response["code"] == "00000") {
                        success = true;
                        // Store for future reference
                        order_types_[order_id] = true;
                    }
                }
            }
            
            // Clean up the map to prevent it from growing too large
            if (success) {
                order_types_.erase(order_id);
            }
            
            return success;
        } catch (const std::exception& e) {
            std::cerr << "Error canceling order on Bitget: " << e.what() << std::endl;
            return false;
        }
    }
    
    OrderStatus getOrderStatus(const std::string& order_id) override {
        try {
            // Check if we have stored the order type
            auto it = order_types_.find(order_id);
            bool is_contract = false;
            bool order_type_known = (it != order_types_.end());
            
            if (order_type_known) {
                is_contract = it->second;
            }
            
            OrderStatus status = OrderStatus::REJECTED; // Default to rejected
            bool status_found = false;
            
            // If we know the order type, try that endpoint first
            if (order_type_known) {
                if (is_contract) {
                    // Try futures endpoint
                    try {
                        std::string futures_endpoint = "/api/mix/v1/order/detail?orderId=" + order_id + "&marginCoin=USDT";
                        json response = makeApiCall(futures_endpoint, "", true);
                        
                        if (response["code"] == "00000" && response.contains("data")) {
                            std::string status_str = response["data"]["state"];
                            
                            // Map Bitget futures status to our internal status
                            if (status_str == "new" || status_str == "init") {
                                status = OrderStatus::NEW;
                            } else if (status_str == "partial_fill") {
                                status = OrderStatus::PARTIALLY_FILLED;
                            } else if (status_str == "filled") {
                                status = OrderStatus::FILLED;
                            } else if (status_str == "cancelled") {
                                status = OrderStatus::CANCELED;
                            } else if (status_str == "rejected") {
                                status = OrderStatus::REJECTED;
                            }
                            status_found = true;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Error getting futures order status: " << e.what() << std::endl;
                    }
                } else {
                    // Try spot endpoint
                    try {
                        std::string spot_endpoint = "/api/spot/v1/trade/orderInfo?orderId=" + order_id;
                        json response = makeApiCall(spot_endpoint, "", true);
                        
                        if (response["code"] == "00000" && response.contains("data")) {
                            std::string status_str = response["data"]["status"];
                            
                            // Map Bitget status to our internal status
                            if (status_str == "new" || status_str == "init") {
                                status = OrderStatus::NEW;
                            } else if (status_str == "partial_fill") {
                                status = OrderStatus::PARTIALLY_FILLED;
                            } else if (status_str == "full_fill") {
                                status = OrderStatus::FILLED;
                            } else if (status_str == "cancelled") {
                                status = OrderStatus::CANCELED;
                            } else if (status_str == "rejected") {
                                status = OrderStatus::REJECTED;
                            }
                            status_found = true;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Error getting spot order status: " << e.what() << std::endl;
                    }
                }
            }
            
            // If order type is unknown or the known endpoint failed, try both endpoints
            if (!status_found) {
                // Try spot first
                try {
                    std::string spot_endpoint = "/api/spot/v1/trade/orderInfo?orderId=" + order_id;
                    json response = makeApiCall(spot_endpoint, "", true);
                    
                    if (response["code"] == "00000" && response.contains("data")) {
                        std::string status_str = response["data"]["status"];
                        
                        // Map Bitget status to our internal status
                        if (status_str == "new" || status_str == "init") {
                            status = OrderStatus::NEW;
                        } else if (status_str == "partial_fill") {
                            status = OrderStatus::PARTIALLY_FILLED;
                        } else if (status_str == "full_fill") {
                            status = OrderStatus::FILLED;
                        } else if (status_str == "cancelled") {
                            status = OrderStatus::CANCELED;
                        } else if (status_str == "rejected") {
                            status = OrderStatus::REJECTED;
                        }
                        
                        // Store for future reference
                        order_types_[order_id] = false;
                        status_found = true;
                    }
                } catch (...) {
                    // If spot query fails, try futures
                }
                
                // Try futures if spot failed
                if (!status_found) {
                    try {
                        std::string futures_endpoint = "/api/mix/v1/order/detail?orderId=" + order_id + "&marginCoin=USDT";
                        json response = makeApiCall(futures_endpoint, "", true);
                        
                        if (response["code"] == "00000" && response.contains("data")) {
                            std::string status_str = response["data"]["state"];
                            
                            // Map Bitget futures status to our internal status
                            if (status_str == "new" || status_str == "init") {
                                status = OrderStatus::NEW;
                            } else if (status_str == "partial_fill") {
                                status = OrderStatus::PARTIALLY_FILLED;
                            } else if (status_str == "filled") {
                                status = OrderStatus::FILLED;
                            } else if (status_str == "cancelled") {
                                status = OrderStatus::CANCELED;
                            } else if (status_str == "rejected") {
                                status = OrderStatus::REJECTED;
                            }
                            
                            // Store for future reference
                            order_types_[order_id] = true;
                            status_found = true;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Error getting futures order status: " << e.what() << std::endl;
                    }
                }
            }
            
            // If the order is in a final state, we can remove it from our tracking map
            if (status == OrderStatus::FILLED || status == OrderStatus::CANCELED || status == OrderStatus::REJECTED) {
                order_types_.erase(order_id);
            }
            
            return status;
        } catch (const std::exception& e) {
            std::cerr << "Error getting order status on Bitget: " << e.what() << std::endl;
            return OrderStatus::REJECTED;
        }
    }
    
    std::vector<Trade> getRecentTrades(const std::string& symbol, int limit = 100) override {
        std::vector<Trade> trades;
        
        try {
            // Determine if this is a spot or contract symbol
            bool is_contract = symbol.find("_UMCBL") != std::string::npos;
            
            std::string endpoint = is_contract ? 
                "/api/mix/v1/market/fills?symbol=" + symbol + "&limit=" + std::to_string(limit) :
                "/api/spot/v1/market/fills?symbol=" + symbol + "&limit=" + std::to_string(limit);
            
            json response = makeApiCall(endpoint, "", false);
            
            if (response["code"] == "00000" && response.contains("data")) {
                for (const auto& trade_data : response["data"]) {
                    Trade trade;
                    trade.symbol = symbol;
                    trade.price = std::stod(trade_data["price"].get<std::string>());
                    trade.amount = std::stod(trade_data["size"].get<std::string>());
                    trade.side = trade_data["side"] == "buy" ? "buy" : "sell";
                    
                    // Convert timestamp
                    if (trade_data.contains("timestamp")) {
                        auto ts = std::stoll(trade_data["timestamp"].get<std::string>());
                        trade.timestamp = std::chrono::system_clock::from_time_t(ts / 1000);
                    } else {
                        trade.timestamp = std::chrono::system_clock::now(); // Fallback to current time
                    }
                    
                    // Get trade ID if available
                    if (trade_data.contains("tradeId")) {
                        trade.trade_id = trade_data["tradeId"];
                    } else {
                        trade.trade_id = "unknown";
                    }
                    
                    trades.push_back(trade);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching recent trades from Bitget: " << e.what() << std::endl;
        }
        
        return trades;
    }
    
    std::vector<Candle> getCandles(const std::string& symbol, 
                                  const std::string& interval,
                                  const std::chrono::system_clock::time_point& start,
                                  const std::chrono::system_clock::time_point& end) override {
        std::vector<Candle> candles;
        
        try {
            // Determine if this is a spot or contract symbol
            bool is_contract = symbol.find("_UMCBL") != std::string::npos;
            
            // Convert interval to Bitget format
            std::string bitget_interval;
            if (interval == "1m") bitget_interval = "1m";
            else if (interval == "5m") bitget_interval = "5m";
            else if (interval == "15m") bitget_interval = "15m";
            else if (interval == "30m") bitget_interval = "30m";
            else if (interval == "1h") bitget_interval = "1H";
            else if (interval == "4h") bitget_interval = "4H";
            else if (interval == "12h") bitget_interval = "12H";
            else if (interval == "1d") bitget_interval = "1D";
            else if (interval == "1w") bitget_interval = "1W";
            else throw std::runtime_error("Unsupported interval: " + interval);
            
            // Convert time to timestamps (in seconds)
            auto start_ts = std::chrono::duration_cast<std::chrono::seconds>(
                start.time_since_epoch()).count();
            auto end_ts = std::chrono::duration_cast<std::chrono::seconds>(
                end.time_since_epoch()).count();
            
            std::string endpoint = is_contract ? 
                "/api/mix/v1/market/candles?symbol=" + symbol + 
                "&granularity=" + bitget_interval +
                "&startTime=" + std::to_string(start_ts * 1000) +
                "&endTime=" + std::to_string(end_ts * 1000) :
                "/api/spot/v1/market/candles?symbol=" + symbol + 
                "&period=" + bitget_interval +
                "&after=" + std::to_string(start_ts * 1000) +
                "&before=" + std::to_string(end_ts * 1000);
            
            json response = makeApiCall(endpoint, "", false);
            
            if (response["code"] == "00000" && response.contains("data")) {
                for (const auto& candle_data : response["data"]) {
                    // Bitget format: [timestamp, open, high, low, close, volume]
                    if (candle_data.size() >= 6) {
                        Candle candle;
                        candle.symbol = symbol;
                        
                        // Parse timestamp
                        auto ts = std::stoll(candle_data[0].get<std::string>());
                        candle.open_time = std::chrono::system_clock::from_time_t(ts / 1000);
                        
                        // Parse OHLCV data
                        candle.open = std::stod(candle_data[1].get<std::string>());
                        candle.high = std::stod(candle_data[2].get<std::string>());
                        candle.low = std::stod(candle_data[3].get<std::string>());
                        candle.close = std::stod(candle_data[4].get<std::string>());
                        candle.volume = std::stod(candle_data[5].get<std::string>());
                        
                        // Bitget doesn't provide number of trades
                        candle.trades = 0;
                        
                        candles.push_back(candle);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error fetching candles from Bitget: " << e.what() << std::endl;
        }
        
        return candles;
    }
    
    // Utility functions
    bool isConnected() override {
        try {
            // Make a simple API call to test connectivity
            std::string endpoint = "/api/spot/v1/public/time";
            makeApiCall(endpoint, "", false);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Bitget connection check failed: " << e.what() << std::endl;
            return false;
        }
    }
    
    bool reconnect() override {
        try {
            curl_global_cleanup();
            curl_global_init(CURL_GLOBAL_ALL);
            
            // Test the connection after re-initializing
            return isConnected();
        } catch (const std::exception& e) {
            std::cerr << "Bitget reconnection failed: " << e.what() << std::endl;
            return false;
        }
    }

private:
    std::string api_key_;
    std::string api_secret_;
    std::string passphrase_; // Bitget requires a passphrase
    std::string base_url_;
    bool use_testnet_;
    FeeStructure fee_structure_;
    std::map<std::string, std::pair<double, double>> symbol_fees_; // symbol -> (maker, taker)
    std::chrono::system_clock::time_point last_fee_update_;
    
    // Map to store order types: order_id -> is_contract (true for futures, false for spot)
    std::map<std::string, bool> order_types_;
    
    // Generate Bitget API signature - using HMAC-SHA256
    std::string generateSignature(const std::string& timestamp, 
                                 const std::string& method,
                                 const std::string& request_path,
                                 const std::string& body = "") {
        // Create the string to sign: timestamp + method + requestPath + body
        std::string message = timestamp + method + request_path + body;
        
        // Prepare the HMAC-SHA256 key and result buffer
        unsigned char* digest = HMAC(EVP_sha256(),
                                   api_secret_.c_str(), api_secret_.length(),
                                   reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
                                   nullptr, nullptr);
        
        // Convert to base64
        char base64_digest[1024] = {0};
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64_digest),
                        digest, 32);  // SHA-256 produces 32 bytes
        
        return std::string(base64_digest);
    }
    
    // Make API call to Bitget
    json makeApiCall(const std::string& endpoint, 
                    const std::string& request_body = "", 
                    bool is_private = false, 
                    const std::string& method = "GET") {
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
            
            // Add authentication headers
            headers = curl_slist_append(headers, ("ACCESS-KEY: " + api_key_).c_str());
            headers = curl_slist_append(headers, ("ACCESS-SIGN: " + signature).c_str());
            headers = curl_slist_append(headers, ("ACCESS-TIMESTAMP: " + timestamp).c_str());
            headers = curl_slist_append(headers, ("ACCESS-PASSPHRASE: " + passphrase_).c_str());
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
    void updateFeeStructure() {
        try {
            // Get general fee tier structure
            std::string endpoint = "/api/spot/v1/account/vipLevel";
            json response = makeApiCall(endpoint, "", true);
            
            if (response["code"] == "00000" && response.contains("data")) {
                auto& data = response["data"];
                
                // Update fee structure from VIP level data
                fee_structure_.fee_tier = data["level"];
                fee_structure_.tier_name = "VIP " + std::to_string(fee_structure_.fee_tier);
                
                // Parse maker/taker fees (convert from percentage to decimal)
                if (data.contains("makerFeeRate")) {
                    fee_structure_.maker_fee = std::stod(data["makerFeeRate"].get<std::string>()) / 100.0;
                    fee_structure_.spot_maker_fee = fee_structure_.maker_fee;
                }
                
                if (data.contains("takerFeeRate")) {
                    fee_structure_.taker_fee = std::stod(data["takerFeeRate"].get<std::string>()) / 100.0;
                    fee_structure_.spot_taker_fee = fee_structure_.taker_fee;
                }
                
                std::cout << "Updated Bitget fee structure: maker=" << fee_structure_.maker_fee 
                          << ", taker=" << fee_structure_.taker_fee << std::endl;
            }
            
            // Get futures fee structure
            endpoint = "/api/mix/v1/account/account-setting";
            response = makeApiCall(endpoint, "", true);
            
            if (response["code"] == "00000" && response.contains("data")) {
                auto& futures_data = response["data"];
                
                // Parse futures maker/taker fees
                if (futures_data.contains("makerFeeRate") && futures_data.contains("takerFeeRate")) {
                    fee_structure_.perp_maker_fee = std::stod(futures_data["makerFeeRate"].get<std::string>()) / 100.0;
                    fee_structure_.perp_taker_fee = std::stod(futures_data["takerFeeRate"].get<std::string>()) / 100.0;
                } else {
                    // Use default fees from spot if futures fees aren't available
                    fee_structure_.perp_maker_fee = fee_structure_.maker_fee;
                    fee_structure_.perp_taker_fee = fee_structure_.taker_fee;
                }
                
                // Use same fees for margin trading (could be different in reality)
                fee_structure_.margin_maker_fee = fee_structure_.maker_fee;
                fee_structure_.margin_taker_fee = fee_structure_.taker_fee;
            }
            
            // Get withdrawal fees (would require another API call)
            // Since Bitget doesn't have a simple endpoint for all withdrawal fees,
            // we'll set some common ones manually for now
            fee_structure_.withdrawal_fees["BTC"] = 0.0005;
            fee_structure_.withdrawal_fees["ETH"] = 0.003;
            fee_structure_.withdrawal_fees["USDT"] = 2.0;
            fee_structure_.withdrawal_fees["USDC"] = 2.0;
            
        } catch (const std::exception& e) {
            std::cerr << "Error updating Bitget fee structure: " << e.what() << std::endl;
            
            // Set default conservative fees
            fee_structure_.maker_fee = 0.0010;     // 0.10%
            fee_structure_.taker_fee = 0.0015;     // 0.15%
            fee_structure_.spot_maker_fee = 0.0010;
            fee_structure_.spot_taker_fee = 0.0015;
            fee_structure_.perp_maker_fee = 0.0010;
            fee_structure_.perp_taker_fee = 0.0015;
            fee_structure_.margin_maker_fee = 0.0015;
            fee_structure_.margin_taker_fee = 0.0020;
            fee_structure_.fee_tier = 0;
            fee_structure_.tier_name = "Default";
        }
    }
};

// Factory function declaration
std::shared_ptr<ExchangeInterface> createBitgetExchange(const ExchangeConfig& config);

} // namespace funding

// Factory function implementation
std::shared_ptr<funding::ExchangeInterface> funding::createBitgetExchange(const funding::ExchangeConfig& config) {
    return std::make_shared<funding::BitgetExchange>(config);
}