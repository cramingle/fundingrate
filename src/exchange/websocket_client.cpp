#include "websocket_client.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <chrono>

namespace funding {

using json = nlohmann::json;

// Initialize static member
WebSocketClient* WebSocketClient::instance_ = nullptr;

// Define protocols array
const struct lws_protocols WebSocketClient::protocols[] = {
    {
        "binance-websocket-protocol",
        WebSocketClient::callback_function,
        0,
        4096,
    },
    { nullptr, nullptr, 0, 0 }
};

WebSocketClient::WebSocketClient(const std::string& url, const std::vector<std::string>& streams)
    : context(nullptr)
    , wsi(nullptr)
    , url_(url)
    , streams_(streams)
    , connected(false) {
    
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&info);
    if (!context) {
        throw std::runtime_error("Failed to create libwebsockets context");
    }
    instance_ = this;
}

WebSocketClient::~WebSocketClient() {
    disconnect();
    if (context) {
        lws_context_destroy(context);
    }
    instance_ = nullptr;
}

bool WebSocketClient::connect(const std::string& url) {
    if (!context || connected) return false;

    struct lws_client_connect_info info;
    memset(&info, 0, sizeof(info));
    info.context = context;
    info.address = "stream.binance.com";
    info.port = 443;
    info.path = "/stream";
    info.host = info.address;
    info.origin = info.address;
    info.protocol = protocols[0].name;
    info.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    info.userdata = this;
    info.pwsi = &wsi;

    wsi = lws_client_connect_via_info(&info);
    if (!wsi) {
        std::cerr << "Failed to connect to WebSocket server" << std::endl;
        return false;
    }

    // Service the WebSocket context until we're connected
    int retries = 0;
    while (!connected && retries < 10) {
        lws_service(context, 50);
        retries++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (connected) {
        // Send subscription message
        json subscribe_msg = {
            {"method", "SUBSCRIBE"},
            {"params", streams_},
            {"id", 1}
        };
        
        std::string msg = subscribe_msg.dump();
        unsigned char* buf = new unsigned char[LWS_PRE + msg.length()];
        memcpy(buf + LWS_PRE, msg.c_str(), msg.length());
        
        lws_write(wsi, buf + LWS_PRE, msg.length(), LWS_WRITE_TEXT);
        delete[] buf;
    }

    return connected;
}

void WebSocketClient::disconnect() {
    if (wsi) {
        lws_callback_on_writable(wsi);
        lws_cancel_service(context);
        wsi = nullptr;
    }
    connected = false;
}

bool WebSocketClient::isConnected() const {
    return connected;
}

void WebSocketClient::setMessageCallback(MessageCallback callback) {
    messageCallback = callback;
}

double WebSocketClient::getLatestPrice(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(priceMutex);
    auto it = latestPrices.find(symbol);
    return it != latestPrices.end() ? it->second : 0.0;
}

int WebSocketClient::callback_function(struct lws *wsi, enum lws_callback_reasons reason,
                                     void *user, void *in, size_t len) {
    WebSocketClient* client = instance_;
    if (!client) return 0;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            std::cout << "WebSocket connection established" << std::endl;
            client->connected = true;
            break;
        }
        
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            // Null-terminate the received data
            std::string payload(static_cast<char*>(in), len);
            
            try {
                json data = json::parse(payload);
                
                // Handle subscription response
                if (data.contains("result") && data.contains("id")) {
                    std::cout << "Subscription successful" << std::endl;
                    break;
                }
                
                // Handle data messages
                if (data.contains("stream") && data.contains("data")) {
                    const auto& streamData = data["data"];
                    if (streamData.contains("s") && streamData.contains("c")) {
                        std::string symbol = streamData["s"].get<std::string>();
                        double price = std::stod(streamData["c"].get<std::string>());
                        
                        {
                            std::lock_guard<std::mutex> lock(client->priceMutex);
                            client->latestPrices[symbol] = price;
                        }
                        
                        if (client->messageCallback) {
                            client->messageCallback(symbol, price);
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Error parsing WebSocket message: " << e.what() << std::endl;
                std::cerr << "Raw message: " << payload << std::endl;
            }
            break;
        }
        
        case LWS_CALLBACK_CLIENT_CLOSED: {
            std::cout << "WebSocket connection closed" << std::endl;
            client->connected = false;
            break;
        }
        
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            std::string error = in ? std::string(static_cast<char*>(in), len) : "Unknown error";
            std::cerr << "WebSocket connection error: " << error << std::endl;
            client->connected = false;
            break;
        }
            
        default:
            break;
    }
    
    return 0;
}

void WebSocketClient::subscribeToStreams(const std::vector<std::string>& streams) {
    if (!isConnected()) {
        std::cerr << "Cannot subscribe to streams: WebSocket not connected" << std::endl;
        return;
    }
    
    // Add new streams to the existing streams list
    for (const auto& stream : streams) {
        if (std::find(streams_.begin(), streams_.end(), stream) == streams_.end()) {
            streams_.push_back(stream);
        }
    }
    
    // Create subscription message in JSON format
    json subscription;
    subscription["method"] = "SUBSCRIBE";
    subscription["params"] = streams;
    subscription["id"] = static_cast<int>(time(nullptr)); // Use current timestamp as ID
    
    std::string message = subscription.dump();
    
    // Send the subscription message
    int len = message.length();
    unsigned char* buf = static_cast<unsigned char*>(malloc(LWS_PRE + len));
    if (!buf) {
        std::cerr << "Out of memory for subscription message" << std::endl;
        return;
    }
    
    memcpy(buf + LWS_PRE, message.c_str(), len);
    
    // Queue the message for sending
    int result = lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
    free(buf);
    
    if (result < 0) {
        std::cerr << "Error sending subscription message" << std::endl;
        return;
    }
    
    std::cout << "Subscribed to " << streams.size() << " additional streams" << std::endl;
}

} // namespace funding 