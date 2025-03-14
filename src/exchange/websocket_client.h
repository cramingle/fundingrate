#pragma once

#include <libwebsockets.h>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <mutex>
#include <memory>

namespace funding {

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&, double)>;
    
    WebSocketClient(const std::string& url, const std::vector<std::string>& streams);
    ~WebSocketClient();
    
    bool connect(const std::string& url);
    void disconnect();
    bool isConnected() const;
    
    void setMessageCallback(MessageCallback callback);
    
    // Get latest price for a symbol
    double getLatestPrice(const std::string& symbol) const;
    
private:
    static int callback_function(struct lws *wsi, enum lws_callback_reasons reason,
                               void *user, void *in, size_t len);
    
    static const struct lws_protocols protocols[];
    
    struct lws_context *context;
    struct lws *wsi;
    std::string url_;
    std::vector<std::string> streams_;
    MessageCallback messageCallback;
    std::map<std::string, double> latestPrices;
    mutable std::mutex priceMutex;
    bool connected;
    
    // Static callback wrapper
    static WebSocketClient* instance_;
};

} // namespace funding 