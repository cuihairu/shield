#include <chrono>
#include <iostream>
#include <thread>

#include "shield/core/logger.hpp"
#include "shield/core/reactor.hpp"

using namespace shield::core;
using namespace std::chrono_literals;

void heavy_computation(int id) {
    SHIELD_LOG_INFO << "Task " << id << " started in thread "
                    << std::this_thread::get_id();

    // Simulate time-consuming computation
    std::this_thread::sleep_for(2s);

    SHIELD_LOG_INFO << "Task " << id << " completed in thread "
                    << std::this_thread::get_id();
}

int main() {
    // Configure logging system
    LogConfig log_config;
    log_config.level = 1;  // Corresponds to debug level
    log_config.log_file = "logs/reactor_example.log";  // Set log file path
    log_config.console_output = true;                  // Also output to console

    // Initialize logging system
    Logger::init(log_config);

    // Create a Reactor with 4 worker threads
    Reactor reactor(4);

    SHIELD_LOG_INFO << "Main thread: " << std::this_thread::get_id();

    // Submit 10 tasks
    for (int i = 0; i < 10; ++i) {
        SHIELD_LOG_DEBUG << "Submitting task " << i;
        reactor.submit_task([i]() { heavy_computation(i); });
    }

    // Run event loop in main thread
    reactor.run();

    // Shutdown logging system
    Logger::shutdown();

    return 0;
}