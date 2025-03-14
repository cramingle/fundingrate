#pragma once

#include <memory>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <config/config_manager.h>
#include <risk/risk_manager.h>
#include <exchange/exchange_interface.h>
#include <strategy/arbitrage_strategy.h>
#include <exchange/websocket_client.h>
#include <set>

namespace funding {

// Main funding rate arbitrage bot
class FundingBot {
public:
    FundingBot(const std::string& config_file);
    ~FundingBot();
    
    // Initialize the bot
    bool initialize();
    
    // Start the bot
    bool start();
    
    // Stop the bot
    bool stop();
    
    // Check if the bot is running
    bool isRunning() const;
    
    // Get active positions
    std::vector<ArbitragePosition> getActivePositions() const;
    
    // Get historical performance
    struct PerformanceStats {
        int total_trades;
        int profitable_trades;
        double total_profit;
        double max_drawdown;
        double sharpe_ratio;
        double annualized_return;
        std::vector<double> daily_returns;  // Store daily returns for Sharpe ratio calculation
    };
    
    PerformanceStats getPerformance() const;
    
    // Get latest price for a symbol
    double getLatestPrice(const std::string& symbol) const;

private:
    // Configuration
    std::string config_file_;
    std::unique_ptr<ConfigManager> config_manager_;
    std::unique_ptr<RiskManager> risk_manager_;
    
    // Exchange connections
    std::map<std::string, std::shared_ptr<ExchangeInterface>> exchanges_;
    
    // WebSocket client for real-time price updates
    std::unique_ptr<WebSocketClient> websocket_client_;
    std::map<std::string, double> latest_prices_;
    mutable std::mutex price_mutex_;
    
    // Strategy instances
    std::vector<std::unique_ptr<ArbitrageStrategy>> strategies_;
    
    // Threading
    std::atomic<bool> running_;
    std::thread main_thread_;
    std::thread monitor_thread_;
    std::thread websocket_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    // Performance tracking
    PerformanceStats performance_;
    std::chrono::system_clock::time_point last_performance_update_;
    
    // Private methods
    void mainLoop();
    void monitorLoop();
    void websocketLoop();
    void scanForOpportunities();
    bool processOpportunity(const ArbitrageOpportunity& opportunity);
    void monitorPositions();
    void updatePerformanceStats();
    void saveState();
    void loadSavedState();
    void setupSignalHandlers();
    void disconnectExchanges();
    void loadStrategies();
    bool connectExchanges();
    
    // WebSocket callbacks
    void onPriceUpdate(const std::string& symbol, double price);
    void setupWebSocketClient();
    void subscribeToSymbols();
    
    // Performance tracking
    void addDailyReturn(double return_value);
    void savePerformanceStats();
    void loadPerformanceStats();
};

} // namespace funding 