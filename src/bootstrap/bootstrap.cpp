// [SHIELD_BOOTSTRAP] Bootstrap implementation
#include "shield/bootstrap/bootstrap.hpp"
#include "shield/bootstrap/starter.hpp"

#include "shield/base/error.hpp"
#include "shield/log_new/logger.hpp"
#include "shield/config_new/config.hpp"
#include "shield/core_new/caf_adapter.hpp"

#include <caf/actor_system.hpp>
#include <caf/io/all.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>

namespace shield::bootstrap {

// Global state
struct GlobalState {
    RuntimeConfig config;
    std::unique_ptr<caf::actor_system> actor_system;
    std::unique_ptr<CafAdapter> caf_adapter;
    bool initialized = false;
};

static GlobalState* g_state = nullptr;
static std::unique_ptr<GlobalState> g_state_owner;

// Initialize
bool initialize(const RuntimeConfig& config) {
    if (g_state && g_state->initialized) {
        return true;  // Already initialized
    }

    g_state_owner = std::make_unique<GlobalState>();
    g_state = g_state_owner.get();
    g_state->config = config;

    // Initialize logging
    shield::log::Logger::initialize();
    shield::log::Logger::set_global_level(
        shield::log::Level::Info);  // Would parse config.log_level

    auto& log = shield::log::get_logger("bootstrap");
    SHIELD_LOG_INFO(log, "Shield runtime initializing...");

    // Run PRE_INIT starters
    run_starters(Phase::PRE_INIT);

    // Initialize CAF actor system
    caf::actor_system_config& caf_config = [&]() -> auto& {
        static caf::actor_system_config cfg;
        cfg.load<caf::io::middleman>();
        return cfg;
    }();

    // Set number of workers
    if (config.num_workers > 0) {
        // caf_config.set("scheduler.max-threads", config.num_workers);
    }

    g_state->actor_system = std::make_unique<caf::actor_system>(caf_config);

    // Initialize CAF adapter
    g_state->caf_adapter = initialize_core(*g_state->actor_system);

    SHIELD_LOG_INFO(log, "CAF actor system initialized");

    // Run POST_SYSTEM_INIT starters
    run_starters(Phase::POST_SYSTEM_INIT);

    // Load config
    if (!config.config_file.empty()) {
        if (shield::config::initialize_config(config.config_file)) {
            SHIELD_LOG_INFO(log, "Config loaded: " + config.config_file);
        } else {
            SHIELD_LOG_WARNING(log, "Failed to load config: " + config.config_file);
        }
    }

    // Run POST_CONFIG starters
    run_starters(Phase::POST_CONFIG);

    // Run POST_START starters
    run_starters(Phase::POST_START);

    g_state->initialized = true;
    SHIELD_LOG_INFO(log, "Shield runtime initialized");
    return true;
}

// Shutdown
void shutdown() {
    if (!g_state || !g_state->initialized) {
        return;
    }

    auto& log = shield::log::get_logger("bootstrap");
    SHIELD_LOG_INFO(log, "Shield runtime shutting down...");

    // Run PRE_SHUTDOWN starters
    run_starters(Phase::PRE_SHUTDOWN);

    // Shutdown actor system (which stops all actors)
    g_state->caf_adapter.reset();
    g_state->actor_system.reset();

    // Run POST_SHUTDOWN starters
    run_starters(Phase::POST_SHUTDOWN);

    // Shutdown logging
    shield::log::Logger::shutdown();

    g_state->initialized = false;
    SHIELD_LOG_INFO(log, "Shield runtime shutdown complete");

    g_state_owner.reset();
    g_state = nullptr;
}

// Check if initialized
bool is_initialized() {
    return g_state && g_state->initialized;
}

// Main entry point
int run(int argc, char** argv) {
    RuntimeConfig config;

    // Parse command line (simplified)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config.config_file = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            config.log_level = argv[++i];
        } else if (arg == "--workers" && i + 1 < argc) {
            config.num_workers = std::stoi(argv[++i]);
        }
    }

    if (!initialize(config)) {
        return 1;
    }

    // Keep running until interrupt
    auto& log = shield::log::get_logger("bootstrap");
    SHIELD_LOG_INFO(log, "Shield runtime running (press Ctrl+C to stop)");

    // Wait for signal (simplified)
    std::this_thread::sleep_for(std::chrono::hours(24 * 365));

    shutdown();
    return 0;
}

// Get global actor system
caf::actor_system* actor_system() {
    return g_state ? g_state->actor_system.get() : nullptr;
}

// Get global CAF adapter
CafAdapter* caf_adapter() {
    return g_state ? g_state->caf_adapter.get() : nullptr;
}

}  // namespace shield::bootstrap
