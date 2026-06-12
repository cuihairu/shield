// [SHIELD] Main entry point
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "shield/bootstrap/bootstrap.hpp"

// Signal handler for graceful shutdown
static std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signal) {
    g_shutdown_requested = true;
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        // Initialize Shield runtime
        shield::bootstrap::RuntimeConfig config;

        // Parse command line args (simplified)
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc) {
                config.config_file = argv[++i];
            } else if (arg == "--log-level" && i + 1 < argc) {
                config.log_level = argv[++i];
            } else if (arg == "--workers" && i + 1 < argc) {
                config.num_workers = std::stoi(argv[++i]);
            } else if (arg == "--help") {
                std::cout << "Shield Game Server Runtime\n"
                          << "Usage: shield [options]\n"
                          << "Options:\n"
                          << "  --config <file>    Config file path\n"
                          << "  --log-level <lvl>  Log level (debug,info,warn,error)\n"
                          << "  --workers <n>      Number of worker threads\n"
                          << "  --help             Show this message\n";
                return 0;
            }
        }

        if (!shield::bootstrap::initialize(config)) {
            std::cerr << "Failed to initialize Shield runtime" << std::endl;
            return 1;
        }

        std::cout << "Shield runtime running (press Ctrl+C to stop)" << std::endl;

        // Wait for shutdown signal
        while (!g_shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Shutting down..." << std::endl;
        shield::bootstrap::shutdown();

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
