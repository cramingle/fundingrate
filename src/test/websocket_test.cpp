#include "../exchange/websocket_client.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

using namespace funding;
using json = nlohmann::json;

void onMessage(const std::string& symbol, double price) {
    std::cout << "Received price for " << symbol << ": " << price << std::endl;
}

int main() {
    std::vector<std::string> streams = {
        "btcusdt@ticker",
        "ethusdt@ticker",
        "xrpusdt@ticker",
        "bnbusdt@ticker",
        "solusdt@ticker"
    };
    
    // Create WebSocket client with the correct stream URL
    WebSocketClient client("wss://stream.binance.com:9443/stream", streams);
    
    // Set message callback before connecting
    client.setMessageCallback(onMessage);
    
    // Connect to the WebSocket server
    if (!client.connect("wss://stream.binance.com:9443/stream")) {
        std::cerr << "Failed to connect to WebSocket server" << std::endl;
        return 1;
    }
    
    std::cout << "Connected to Binance WebSocket server. Waiting for price updates..." << std::endl;
    
    // Keep the connection alive for 30 seconds
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - start).count() < 30) {
        if (client.isConnected()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } else {
            std::cout << "Connection lost" << std::endl;
            break;
        }
    }
    
    std::cout << "Test complete. Disconnecting..." << std::endl;
    client.disconnect();
    return 0;
} 