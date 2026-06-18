// [SHIELD_BOOTSTRAP] Bootstrap implementation
#include "shield/bootstrap/bootstrap.hpp"
#include "shield/bootstrap/starter.hpp"

#include "shield/base/error.hpp"
#include "shield/log/logger.hpp"
#include "shield/config/config.hpp"
#include "shield/core/caf_adapter.hpp"
#include "shield/caf_initializer.hpp"
#include "shield/data/data.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/shield.hpp"

#include <nlohmann/json.hpp>

#include <caf/actor_system.hpp>
#include <caf/io/all.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace shield::bootstrap {
using shield::core::CafAdapter;
using shield::core::initialize_core;

namespace {

std::string resolve_script_path(const shield::config::RuntimeActorConfig& actor) {
    std::filesystem::path script(actor.script);
    if (script.is_absolute() || std::filesystem::exists(script)) {
        return script.string();
    }

    if (!actor.source_dir.empty()) {
        auto from_config = std::filesystem::path(actor.source_dir) / script;
        if (std::filesystem::exists(from_config)) {
            return from_config.string();
        }
    }

    return script.string();
}

}  // namespace

// Global state
struct GlobalState {
    RuntimeConfig config;
    std::unique_ptr<caf::actor_system> actor_system;
    std::unique_ptr<CafAdapter> caf_adapter;
    std::unique_ptr<shield::lua::LuaRuntime> lua_runtime;
    std::unique_ptr<shield::lua::LuaServiceManager> lua_services;
    bool initialized = false;
};

static GlobalState* g_state = nullptr;
static std::unique_ptr<GlobalState> g_state_owner;

void cleanup_failed_initialize() {
    if (g_state) {
        if (g_state->lua_services) {
            g_state->lua_services->shutdown_all("startup_failed");
        }
        g_state->lua_services.reset();
        g_state->lua_runtime.reset();
        g_state->caf_adapter.reset();
        g_state->actor_system.reset();
        g_state->initialized = false;
    }
    shield::log::Logger::shutdown();
    g_state_owner.reset();
    g_state = nullptr;
}

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

    // Load config files in CLI order. Later files override earlier values.
    shield::config::reset_config();
    std::vector<std::string> config_files = config.config_files;
    if (config_files.empty() && !config.config_file.empty()) {
        config_files.push_back(config.config_file);
    }
    for (const auto& config_file : config_files) {
        if (config_file.empty()) {
            continue;
        }
        if (shield::config::initialize_config(config_file)) {
            SHIELD_LOG_INFO(log, "Config loaded: " + config_file);
        } else {
            SHIELD_LOG_ERROR(log, "Failed to load config: " + config_file);
            cleanup_failed_initialize();
            return false;
        }
    }

    shield::config::RuntimeValidationOptions validation_options;
    validation_options.cluster_enabled = false;
    validation_options.global_enabled = false;
    validation_options.player_enabled = false;
    validation_options.server_enabled = false;
    validation_options.ops_enabled = false;

    std::string validation_error;
    if (!shield::config::validate_runtime_config(validation_options,
                                                 &validation_error)) {
        SHIELD_LOG_ERROR(log, "Invalid config: " + validation_error);
        cleanup_failed_initialize();
        return false;
    }

    // Run POST_CONFIG starters before runtime systems consume config.
    run_starters(Phase::POST_CONFIG);

    if (shield::config::get_bool("database.enabled", false)) {
        if (!shield::data::database().initialize()) {
            SHIELD_LOG_ERROR(log, "Failed to initialize database pool");
            cleanup_failed_initialize();
            return false;
        }
        SHIELD_LOG_INFO(log, "Database pool initialized");
    }

    if (shield::config::get_bool("redis.enabled", false)) {
        if (!shield::data::redis().initialize()) {
            SHIELD_LOG_ERROR(log, "Failed to initialize Redis pool");
            cleanup_failed_initialize();
            return false;
        }
        SHIELD_LOG_INFO(log, "Redis pool initialized");
    }

    // Initialize CAF actor system
    initialize_caf_types();
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

    g_state->lua_runtime = std::make_unique<shield::lua::LuaRuntime>();
    g_state->lua_services =
        std::make_unique<shield::lua::LuaServiceManager>(*g_state->lua_runtime);

    for (const auto& actor : shield::config::runtime_actors()) {
        const int instances = actor.instances < 0 ? 0 : actor.instances;
        for (int i = 0; i < instances; ++i) {
            std::string service_name = actor.name;
            if (instances > 1) {
                service_name += ".";
                service_name += std::to_string(i + 1);
            }

            nlohmann::json opts = {
                {"name", service_name},
                {"args", nlohmann::json::object()},
                {"config", nlohmann::json::parse(actor.options_json, nullptr, false)},
            };
            if (opts["config"].is_discarded()) {
                opts["config"] = nlohmann::json::object();
            }

            auto result = g_state->lua_services->spawn(resolve_script_path(actor),
                                                       opts.dump());
            if (!result.success) {
                SHIELD_LOG_ERROR(log, "Failed to spawn actor '" + service_name +
                                      "': " + result.error_message);
                if (actor.required) {
                    cleanup_failed_initialize();
                    return false;
                }
                continue;
            }
            SHIELD_LOG_INFO(log, "Service spawned: " + result.service_id);
        }
    }

    // Run POST_START starters
    run_starters(Phase::POST_START);

    // Start the Lua worker thread. After this point, all mailbox drains,
    // timer fires, coroutine timeouts, and forked tasks happen on the worker.
    if (g_state->lua_services) {
        g_state->lua_services->start_worker();
    }

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

    // Stop the Lua worker first so no new Lua code runs while we tear down.
    if (g_state->lua_services) {
        g_state->lua_services->stop_worker();
    }

    // Shutdown actor system (which stops all actors)
    if (g_state->lua_services) {
        g_state->lua_services->shutdown_all("stopping");
    }
    g_state->lua_services.reset();
    g_state->lua_runtime.reset();
    g_state->caf_adapter.reset();
    g_state->actor_system.reset();

    // Run POST_SHUTDOWN starters
    run_starters(Phase::POST_SHUTDOWN);

    g_state->initialized = false;
    SHIELD_LOG_INFO(log, "Shield runtime shutdown complete");

    // Shutdown logging after the final runtime log has been emitted.
    shield::log::Logger::shutdown();

    g_state_owner.reset();
    g_state = nullptr;
}

// Check if initialized
bool is_initialized() {
    return g_state && g_state->initialized;
}

int run(int argc, char** argv) {
    return shield::run(argc, argv);
}

}  // namespace shield::bootstrap
