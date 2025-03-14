#include <iostream>
#include <string>
#include <csignal>
#include <core/funding_bot.h>

// Global bot instance for signal handling
funding::FundingBot* g_bot = nullptr;

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ". Shutting down..." << std::endl;
    if (g_bot) {
        g_bot->stop();
    }
}

void setupSignalHandlers() {
    signal(SIGINT, signalHandler);  // Ctrl+C
    signal(SIGTERM, signalHandler); // Termination request
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -c, --config <file>  Path to config file (default: config/bot_config.json)" << std::endl;
    std::cout << "  -h, --help           Show this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string config_file = "config/bot_config.json";
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                std::cerr << "Error: --config option requires a file path" << std::endl;
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    // Setup signal handlers for graceful shutdown
    setupSignalHandlers();
    
    try {
        std::cout << "Starting Funding Rate Arbitrage Bot" << std::endl;
        std::cout << "Using config file: " << config_file << std::endl;
        
        // Create bot instance
        funding::FundingBot bot(config_file);
        g_bot = &bot;
        
        // Initialize the bot
        if (!bot.initialize()) {
            std::cerr << "Failed to initialize bot" << std::endl;
            return 1;
        }
        
        // Start the bot
        if (!bot.start()) {
            std::cerr << "Failed to start bot" << std::endl;
            return 1;
        }
        
        // Main loop - wait for signals
        std::cout << "Bot is running. Press Ctrl+C to stop." << std::endl;
        
        // Wait until bot is stopped
        while (bot.isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        // Print performance stats
        auto stats = bot.getPerformance();
        std::cout << "\nBot stopped. Performance summary:" << std::endl;
        std::cout << "Total trades: " << stats.total_trades << std::endl;
        std::cout << "Profitable trades: " << stats.profitable_trades << " (" 
                 << (stats.total_trades > 0 ? stats.profitable_trades * 100.0 / stats.total_trades : 0) 
                 << "%)" << std::endl;
        std::cout << "Total profit: " << stats.total_profit << " USD" << std::endl;
        std::cout << "Annualized return: " << stats.annualized_return << "%" << std::endl;
        
        g_bot = nullptr;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        g_bot = nullptr;
        return 1;
    }
} 