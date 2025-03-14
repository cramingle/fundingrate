#include <core/funding_bot.h>
#include <config/config_manager.h>
#include <risk/risk_manager.h>
#include <strategy/strategy_factory.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <ctime>
#include <csignal>
#include <filesystem>

namespace funding {

// Global mutex for thread-safe logging
std::mutex g_log_mutex;

// Helper function for thread-safe logging
void log(const std::string& message, bool is_error = false, bool console_output = true) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    ss << (is_error ? " [ERROR] " : " [INFO] ") << message;
    
    // Output to console if requested
    if (console_output) {
        if (is_error) {
            std::cerr << ss.str() << std::endl;
        } else {
            std::cout << ss.str() << std::endl;
        }
    }
    
    // Ensure log directory exists
    std::filesystem::create_directories("logs");
    
    // Write to log file
    std::stringstream date_ss;
    date_ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d");
    std::string filename = "logs/funding_bot_" + date_ss.str() + ".log";
    
    std::ofstream log_file(filename, std::ios_base::app);
    if (log_file.is_open()) {
        log_file << ss.str() << std::endl;
        log_file.close();
    }
}

// Helper function to format currency amounts
std::string formatCurrency(double amount, int precision = 2) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(precision) << amount;
    return ss.str();
}

// Helper function to calculate time difference in a human-readable format
std::string timeDiffToString(const std::chrono::system_clock::time_point& start,
                           const std::chrono::system_clock::time_point& end) {
    auto diff = end - start;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(diff).count();
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(diff).count() % 60;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(diff).count() % 60;
    
    std::stringstream ss;
    if (hours > 0) {
        ss << hours << "h ";
    }
    if (minutes > 0 || hours > 0) {
        ss << minutes << "m ";
    }
    ss << seconds << "s";
    
    return ss.str();
}

// Constructor
FundingBot::FundingBot(const std::string& config_file) 
    : running_(false), 
      config_file_(config_file),
      last_performance_update_(std::chrono::system_clock::now()) {
    
    log("Initializing FundingBot with config file: " + config_file);
    
    try {
        // Create configuration manager
        config_manager_ = std::make_unique<ConfigManager>(config_file);
        
        // Initialize performance stats
        performance_.total_trades = 0;
        performance_.profitable_trades = 0;
        performance_.total_profit = 0.0;
        performance_.max_drawdown = 0.0;
        performance_.sharpe_ratio = 0.0;
        performance_.annualized_return = 0.0;
        
        // Set up signal handlers for graceful shutdown
        // Note: In a real implementation, you'd use a proper signal handling mechanism
        
    } catch (const std::exception& e) {
        log("Error in FundingBot constructor: " + std::string(e.what()), true);
        throw; // Re-throw to allow caller to handle initialization failure
    }
}

// Destructor
FundingBot::~FundingBot() {
    log("FundingBot destructor called");
    
    try {
        if (running_) {
            stop();
        }
        
        // Clean up resources
        disconnectExchanges();
        
        // Save final performance stats
        savePerformanceStats();
        
    } catch (const std::exception& e) {
        log("Error in FundingBot destructor: " + std::string(e.what()), true);
    }
}

// Initialize the bot
bool FundingBot::initialize() {
    log("Initializing FundingBot...");
    
    try {
        // Load configuration
        if (!config_manager_->loadConfig()) {
            log("Failed to load configuration", true);
            return false;
        }
        
        // Create risk manager with configuration
        const auto& risk_config = config_manager_->getRiskConfig();
        risk_manager_ = std::make_unique<RiskManager>(risk_config);
        
        log("Risk manager initialized with max position size: " + 
            formatCurrency(risk_config.max_position_size_usd) + " USD");
        
        // Connect to exchanges
        if (!connectExchanges()) {
            log("Failed to connect to exchanges", true);
            return false;
        }
        
        // Load strategies
        loadStrategies();
        
        // Load any saved state (positions, performance, etc.)
        loadSavedState();
        
        log("FundingBot initialized successfully");
        return true;
    } catch (const std::exception& e) {
        log("Error initializing FundingBot: " + std::string(e.what()), true);
        return false;
    }
}

// Start the bot
bool FundingBot::start() {
    if (running_) {
        log("FundingBot is already running");
        return true;
    }
    
    log("Starting FundingBot...");
    
    try {
        // Set running flag
        running_ = true;
        
        // Start main processing thread
        main_thread_ = std::thread(&FundingBot::mainLoop, this);
        
        // Start monitoring thread
        monitor_thread_ = std::thread(&FundingBot::monitorLoop, this);
        
        log("FundingBot started successfully");
        return true;
    } catch (const std::exception& e) {
        running_ = false;
        log("Error starting FundingBot: " + std::string(e.what()), true);
        return false;
    }
}

// Stop the bot
bool FundingBot::stop() {
    if (!running_) {
        log("FundingBot is not running");
        return true;
    }
    
    log("Stopping FundingBot...");
    
    try {
        // Signal threads to stop
        running_ = false;
        
        // Notify any waiting threads
        cv_.notify_all();
        
        // Wait for threads to finish with timeout
        if (main_thread_.joinable()) {
            main_thread_.join();
        }
        
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
        
        // Save state before stopping
        saveState();
        
        log("FundingBot stopped successfully");
        return true;
    } catch (const std::exception& e) {
        log("Error stopping FundingBot: " + std::string(e.what()), true);
        return false;
    }
}

// Check if the bot is running
bool FundingBot::isRunning() const {
    return running_;
}

// Get active positions
std::vector<ArbitragePosition> FundingBot::getActivePositions() const {
    try {
        if (risk_manager_) {
            return risk_manager_->getActivePositions();
        }
    } catch (const std::exception& e) {
        log("Error getting active positions: " + std::string(e.what()), true);
    }
    return {};
}

// Get performance statistics
FundingBot::PerformanceStats FundingBot::getPerformance() const {
    return performance_;
}

// Main processing loop
void FundingBot::mainLoop() {
    log("Main loop started");
    
    // Track last scan time to control frequency
    auto last_scan_time = std::chrono::system_clock::now();
    
    // Get scan interval from config (default to 60 seconds)
    int scan_interval_seconds = 60;
    try {
        const auto& bot_config = config_manager_->getBotConfig();
        if (!bot_config.strategies.empty()) {
            scan_interval_seconds = bot_config.strategies[0].scan_interval_seconds;
        }
    } catch (const std::exception& e) {
        log("Error getting scan interval from config: " + std::string(e.what()), true);
    }
    
    log("Scan interval set to " + std::to_string(scan_interval_seconds) + " seconds");
    
    while (running_) {
        try {
            // Get current time
            auto now = std::chrono::system_clock::now();
            
            // Check if it's time to scan for opportunities
            if (now - last_scan_time >= std::chrono::seconds(scan_interval_seconds)) {
                // Scan for new opportunities
                scanForOpportunities();
                
                // Update last scan time
                last_scan_time = now;
            }
            
            // Sleep for a short time to avoid excessive CPU usage
            // Use condition variable with timeout to allow for early wakeup
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this]() { return !running_; });
            
        } catch (const std::exception& e) {
            log("Error in main loop: " + std::string(e.what()), true);
            
            // Sleep for a while before retrying to avoid rapid error loops
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    
    log("Main loop stopped");
}

// Monitoring loop
void FundingBot::monitorLoop() {
    log("Monitor loop started");
    
    // Track last monitoring time
    auto last_monitor_time = std::chrono::system_clock::now();
    
    // Track last performance update time
    auto last_performance_update = std::chrono::system_clock::now();
    
    // Track last state save time
    auto last_state_save = std::chrono::system_clock::now();
    
    while (running_) {
        try {
            // Get current time
            auto now = std::chrono::system_clock::now();
            
            // Monitor positions every 30 seconds
            if (now - last_monitor_time >= std::chrono::seconds(30)) {
                monitorPositions();
                last_monitor_time = now;
            }
            
            // Update performance stats every 5 minutes
            if (now - last_performance_update >= std::chrono::minutes(5)) {
                updatePerformanceStats();
                last_performance_update = now;
            }
            
            // Save state every 15 minutes
            if (now - last_state_save >= std::chrono::minutes(15)) {
                saveState();
                last_state_save = now;
            }
            
            // Sleep for a short time to avoid excessive CPU usage
            // Use condition variable with timeout to allow for early wakeup
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this]() { return !running_; });
            
        } catch (const std::exception& e) {
            log("Error in monitor loop: " + std::string(e.what()), true);
            
            // Sleep for a while before retrying to avoid rapid error loops
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    
    log("Monitor loop stopped");
}

// Scan for arbitrage opportunities
void FundingBot::scanForOpportunities() {
    log("Scanning for arbitrage opportunities...");
    
    int total_opportunities = 0;
    int valid_opportunities = 0;
    int executed_opportunities = 0;
    
    try {
        // Scan each strategy for opportunities
        for (const auto& strategy : strategies_) {
            // Find opportunities
            auto opportunities = strategy->findOpportunities();
            total_opportunities += opportunities.size();
            
            // Log the number of opportunities found
            if (!opportunities.empty()) {
                log("Found " + std::to_string(opportunities.size()) + 
                    " potential opportunities");
            }
            
            // Process each opportunity
            for (const auto& opportunity : opportunities) {
                // Validate opportunity with risk manager
                if (risk_manager_->canEnterPosition(opportunity)) {
                    valid_opportunities++;
                    
                    // Process the opportunity
                    if (processOpportunity(opportunity)) {
                        executed_opportunities++;
                    }
                }
            }
        }
        
        // Log summary
        if (total_opportunities > 0) {
            log("Opportunity scan complete: " + 
                std::to_string(total_opportunities) + " total, " +
                std::to_string(valid_opportunities) + " valid, " +
                std::to_string(executed_opportunities) + " executed");
        } else {
            log("No arbitrage opportunities found in this scan");
        }
        
    } catch (const std::exception& e) {
        log("Error scanning for opportunities: " + std::string(e.what()), true);
    }
}

// Process an arbitrage opportunity
bool FundingBot::processOpportunity(const ArbitrageOpportunity& opportunity) {
    // Log opportunity details
    std::stringstream ss;
    ss << "Processing opportunity: " 
       << opportunity.pair.exchange1 << ":" << opportunity.pair.symbol1 
       << " <-> "
       << opportunity.pair.exchange2 << ":" << opportunity.pair.symbol2
       << ", Funding rate: " << (opportunity.net_funding_rate) << "%/year"
       << ", Est. profit: " << formatCurrency(opportunity.estimated_profit, 4) << "%";
    log(ss.str());
    
    try {
        // Double-check with risk manager
        if (!risk_manager_->canEnterPosition(opportunity)) {
            log("Risk manager rejected opportunity");
            return false;
        }
        
        // Find the strategy that can handle this opportunity
        for (const auto& strategy : strategies_) {
            if (strategy->validateOpportunity(opportunity)) {
                // Calculate optimal position size
                double size = strategy->calculateOptimalPositionSize(opportunity);
                
                if (size <= 0) {
                    log("Calculated position size is zero or negative, skipping");
                    return false;
                }
                
                log("Executing trade with size: " + formatCurrency(size) + " USD");
                
                // Execute the trade
                bool success = strategy->executeTrade(opportunity, size);
                
                if (success) {
                    log("Trade executed successfully");
                    
                    // Create position record
                    ArbitragePosition position;
                    position.opportunity = opportunity;
                    position.position_size = size;
                    position.entry_time = std::chrono::system_clock::now();
                    
                    // Get current prices
                    try {
                        // Find the exchanges
                        auto exchange1_it = exchanges_.find(opportunity.pair.exchange1);
                        auto exchange2_it = exchanges_.find(opportunity.pair.exchange2);
                        
                        if (exchange1_it != exchanges_.end() && exchange2_it != exchanges_.end()) {
                            position.entry_price1 = exchange1_it->second->getPrice(opportunity.pair.symbol1);
                            position.entry_price2 = exchange2_it->second->getPrice(opportunity.pair.symbol2);
                            position.current_price1 = position.entry_price1;
                            position.current_price2 = position.entry_price2;
                            position.current_spread = opportunity.entry_price_spread;
                        }
                    } catch (const std::exception& e) {
                        log("Error getting prices for position: " + std::string(e.what()), true);
                    }
                    
                    position.initial_spread = opportunity.entry_price_spread;
                    position.funding_collected = 0.0;
                    position.unrealized_pnl = 0.0;
                    
                    // Generate a unique position ID
                    std::stringstream pos_id;
                    pos_id << opportunity.pair.exchange1 << "_" 
                           << opportunity.pair.symbol1 << "_"
                           << opportunity.pair.exchange2 << "_"
                           << opportunity.pair.symbol2 << "_"
                           << std::chrono::duration_cast<std::chrono::milliseconds>(
                               position.entry_time.time_since_epoch()).count();
                    position.position_id = pos_id.str();
                    position.is_active = true;
                    
                    // Register position with risk manager
                    risk_manager_->registerPosition(position);
                    
                    // Update performance stats
                    performance_.total_trades++;
                    
                    // Save state after new position
                    saveState();
                    
                    return true;
                } else {
                    log("Trade execution failed", true);
                }
                
                break;
            }
        }
        
        log("No strategy could validate and execute this opportunity");
        return false;
        
    } catch (const std::exception& e) {
        log("Error processing opportunity: " + std::string(e.what()), true);
        return false;
    }
}

// Monitor active positions
void FundingBot::monitorPositions() {
    try {
        // Get active positions
        auto positions = risk_manager_->getActivePositions();
        
        if (positions.empty()) {
            return; // No positions to monitor
        }
        
        log("Monitoring " + std::to_string(positions.size()) + " active positions");
        
        // First, let each strategy monitor its positions
        for (const auto& strategy : strategies_) {
            strategy->monitorPositions();
        }
        
        // Then, update position status with latest market data
        for (auto& position : positions) {
            // Update position status
            risk_manager_->updatePositionStatus(position, exchanges_);
            
            // Log position status
            std::stringstream ss;
            ss << "Position " << position.position_id << ": "
               << "Age: " << timeDiffToString(position.entry_time, std::chrono::system_clock::now())
               << ", PnL: " << formatCurrency(position.unrealized_pnl, 2) << " USD"
               << ", Funding collected: " << formatCurrency(position.funding_collected, 2) << " USD";
            log(ss.str());
            
            // Check if position should be closed
            if (risk_manager_->shouldClosePosition(position)) {
                log("Risk manager signals to close position: " + position.position_id);
                
                // Find the strategy that can close this position
                for (const auto& strategy : strategies_) {
                    if (strategy->validateOpportunity(position.opportunity)) {
                        log("Closing position with strategy: " + 
                            position.opportunity.pair.exchange1 + " <-> " + 
                            position.opportunity.pair.exchange2);
                        
                        // Close the position
                        bool closed = strategy->closePosition(position.opportunity);
                        
                        if (closed) {
                            log("Successfully closed position: " + position.position_id);
                            
                            // Update performance stats
                            if (position.unrealized_pnl > 0) {
                                performance_.profitable_trades++;
                            }
                            performance_.total_profit += position.unrealized_pnl;
                            
                            // Save state after position close
                            saveState();
                        } else {
                            log("Failed to close position: " + position.position_id, true);
                        }
                        
                        break;
                    }
                }
            }
            
            // Check if position should be reduced
            double reduce_percent = 0.0;
            if (risk_manager_->shouldReducePosition(position, reduce_percent)) {
                log("Risk manager signals to reduce position by " + 
                    formatCurrency(reduce_percent * 100, 0) + "%: " + position.position_id);
                
                // Implementation for position reduction would go here
                // This would involve finding the right strategy and calling a method to reduce the position
            }
        }
        
    } catch (const std::exception& e) {
        log("Error monitoring positions: " + std::string(e.what()), true);
    }
}

// Update performance statistics
void FundingBot::updatePerformanceStats() {
    try {
        // Get active positions
        auto positions = risk_manager_->getActivePositions();
        
        // Calculate total position value
        double total_position_value = 0.0;
        for (const auto& position : positions) {
            total_position_value += position.position_size;
        }
        
        // Calculate unrealized PnL
        double unrealized_pnl = 0.0;
        for (const auto& position : positions) {
            unrealized_pnl += position.unrealized_pnl;
        }
        
        // Update max drawdown if needed
        double current_equity = performance_.total_profit + unrealized_pnl;
        static double peak_equity = current_equity;
        
        if (current_equity > peak_equity) {
            peak_equity = current_equity;
        } else {
            double drawdown = (peak_equity - current_equity) / peak_equity * 100.0;
            if (drawdown > performance_.max_drawdown) {
                performance_.max_drawdown = drawdown;
            }
        }
        
        // Calculate annualized return
        auto now = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::hours>(
            now - last_performance_update_).count();
        
        if (duration > 0) {
            double hours_per_year = 24.0 * 365.0;
            double years = duration / hours_per_year;
            
            if (years > 0) {
                // Simple annualization formula
                performance_.annualized_return = 
                    (performance_.total_profit / total_position_value) / years * 100.0;
            }
        }
        
        // Calculate Sharpe ratio (simplified)
        // In a real implementation, you would calculate this properly with daily returns
        if (performance_.max_drawdown > 0) {
            performance_.sharpe_ratio = 
                performance_.annualized_return / performance_.max_drawdown;
        }
        
        // Log performance stats
        std::stringstream ss;
        ss << "Performance stats: "
           << "Trades: " << performance_.total_trades
           << ", Profitable: " << performance_.profitable_trades
           << ", Win rate: " << (performance_.total_trades > 0 ? 
                               (double)performance_.profitable_trades / performance_.total_trades * 100.0 : 0.0) << "%"
           << ", Total profit: " << formatCurrency(performance_.total_profit) << " USD"
           << ", Max drawdown: " << formatCurrency(performance_.max_drawdown, 2) << "%"
           << ", Annualized return: " << formatCurrency(performance_.annualized_return, 2) << "%"
           << ", Sharpe ratio: " << formatCurrency(performance_.sharpe_ratio, 2);
        log(ss.str());
        
        // Save performance stats
        savePerformanceStats();
        
        // Update last update time
        last_performance_update_ = now;
        
    } catch (const std::exception& e) {
        log("Error updating performance stats: " + std::string(e.what()), true);
    }
}

// Connect to exchanges
bool FundingBot::connectExchanges() {
    log("Connecting to exchanges...");
    
    try {
        // Create exchange instances from configuration
        exchanges_ = config_manager_->createExchanges();
        
        // Check if we have at least one exchange
        if (exchanges_.empty()) {
            log("No exchanges configured", true);
            return false;
        }
        
        // Log connected exchanges
        std::stringstream ss;
        ss << "Connected to " << exchanges_.size() << " exchanges: ";
        bool first = true;
        for (const auto& [name, exchange] : exchanges_) {
            if (!first) {
                ss << ", ";
            }
            ss << name;
            first = false;
            
            // Check if exchange is connected
            if (!exchange->isConnected()) {
                log("Failed to connect to exchange: " + name, true);
                return false;
            }
        }
        log(ss.str());
        
        return true;
    } catch (const std::exception& e) {
        log("Error connecting to exchanges: " + std::string(e.what()), true);
        return false;
    }
}

// Disconnect from exchanges
void FundingBot::disconnectExchanges() {
    log("Disconnecting from exchanges...");
    exchanges_.clear();
}

// Load strategies
void FundingBot::loadStrategies() {
    log("Loading strategies...");
    
    try {
        // Get exchange vector for strategy creation
        std::vector<std::shared_ptr<ExchangeInterface>> exchange_vec;
        for (const auto& [name, exchange] : exchanges_) {
            exchange_vec.push_back(exchange);
        }
        
        // Create strategies based on configuration
        const auto& bot_config = config_manager_->getBotConfig();
        strategies_ = createAllStrategies(bot_config, exchanges_);
        
        log("Loaded " + std::to_string(strategies_.size()) + " strategies");
        
        // Log strategy details
        for (size_t i = 0; i < strategies_.size(); ++i) {
            log("Strategy " + std::to_string(i+1) + " loaded");
        }
        
    } catch (const std::exception& e) {
        log("Error loading strategies: " + std::string(e.what()), true);
        throw; // Re-throw to allow caller to handle initialization failure
    }
}

// Save current state
void FundingBot::saveState() {
    try {
        // Ensure directory exists
        std::filesystem::create_directories("data");
        
        // Save performance stats
        savePerformanceStats();
        
        // Save active positions
        auto positions = risk_manager_->getActivePositions();
        
        // In a real implementation, you would serialize the positions to a file
        // For this example, we'll just log the number of positions saved
        log("Saved state with " + std::to_string(positions.size()) + " active positions");
        
    } catch (const std::exception& e) {
        log("Error saving state: " + std::string(e.what()), true);
    }
}

// Load saved state
void FundingBot::loadSavedState() {
    try {
        // In a real implementation, you would deserialize the state from a file
        // For this example, we'll just log that we're loading state
        log("Loading saved state...");
        
        // Load performance stats
        loadPerformanceStats();
        
    } catch (const std::exception& e) {
        log("Error loading saved state: " + std::string(e.what()), true);
    }
}

// Save performance statistics
void FundingBot::savePerformanceStats() {
    try {
        // Ensure directory exists
        std::filesystem::create_directories("data");
        
        // In a real implementation, you would serialize the performance stats to a file
        // For this example, we'll just log that we're saving performance stats
        log("Saving performance statistics...");
        
    } catch (const std::exception& e) {
        log("Error saving performance stats: " + std::string(e.what()), true);
    }
}

// Load performance statistics
void FundingBot::loadPerformanceStats() {
    try {
        // In a real implementation, you would deserialize the performance stats from a file
        // For this example, we'll just log that we're loading performance stats
        log("Loading performance statistics...");
        
    } catch (const std::exception& e) {
        log("Error loading performance stats: " + std::string(e.what()), true);
    }
}

} // namespace funding 