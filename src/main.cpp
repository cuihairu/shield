#include <unistd.h>  // for gethostname(), getpid()

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

// Initialize CAF type system BEFORE including other headers
#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/init_global_meta_objects.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/actor/lua_actor.hpp"
#include "shield/caf_type_ids.hpp"
#include "shield/core/command_line_parser.hpp"
#include "shield/core/component.hpp"
#include "shield/core/config.hpp"
#include "shield/core/logger.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/gateway/gateway_component.hpp"
#include "shield/metrics/prometheus_component.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/serialization/universal_serialization_system.hpp"
#include "shield/version.hpp"

// Global flag for signal handling
volatile sig_atomic_t g_signal_status = 0;

void signal_handler(int signal) { g_signal_status = signal; }

int main(int argc, char* argv[]) {
    // Set up signal handling first
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initial minimal logging
    shield::core::LogConfig log_config;
    shield::core::Logger::init(log_config);

    auto options = shield::core::CommandLineParser::parse(argc, argv);

    if (options.show_help || options.show_version) {
        std::cout << "Shield Game Framework v" << shield::VERSION
                  << " (Git Commit: " << GIT_COMMIT_HASH << ")" << std::endl;
        return 0;
    }

    std::cout << "Shield Game Framework v" << shield::VERSION
              << " (Git Commit: " << GIT_COMMIT_HASH << ")" << std::endl;

    // Load configuration
    auto& config = shield::core::Config::instance();
    try {
        config.load(options.config_file.empty() ? "config/shield.yaml"
                                                : options.config_file);
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to load configuration: " << e.what();
        return 1;
    }

    // Re-initialize logger with config values
    log_config.level = shield::core::Logger::level_from_string(
        config.get<std::string>("logger.level"));
    log_config.console_output = config.get<bool>("logger.console_output");
    shield::core::Logger::init(log_config);

    SHIELD_LOG_INFO << "Application starting...";

    // Initialize serialization system
    shield::serialization::initialize_universal_serialization_system();

    // Initialize CAF global meta objects for core types
    caf::core::init_global_meta_objects();

    // Initialize CAF actor system
    caf::actor_system_config caf_cfg;
    caf::actor_system system{caf_cfg};

    // Service Discovery
    auto discovery_service_unique = shield::discovery::make_local_discovery();
    std::shared_ptr<shield::discovery::IServiceDiscovery> discovery_service =
        std::move(discovery_service_unique);

    // Distributed Actor System
    shield::actor::DistributedActorConfig actor_config;

    // Configure dynamic node ID
    std::string node_id_config =
        config.get<std::string>("actor_system.node_id");
    if (node_id_config == "auto") {
        // Generate unique node ID based on hostname and process ID
        std::string hostname = "unknown";
        char hostname_buffer[256];
        if (gethostname(hostname_buffer, sizeof(hostname_buffer)) == 0) {
            hostname = std::string(hostname_buffer);
        }

        auto process_id = getpid();
        auto timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        actor_config.node_id = hostname + "_" + std::to_string(process_id) +
                               "_" + std::to_string(timestamp);
        SHIELD_LOG_INFO << "Generated dynamic node ID: "
                        << actor_config.node_id;
    } else {
        // Use configured node ID
        actor_config.node_id = node_id_config;
        SHIELD_LOG_INFO << "Using configured node ID: " << actor_config.node_id;
    }

    shield::actor::DistributedActorSystem distributed_actor_system(
        system, discovery_service, actor_config);
    distributed_actor_system.initialize();

    // Component management
    std::vector<std::unique_ptr<shield::core::Component>> components;
    auto component_names = config.get<std::vector<std::string>>("components");

    for (const auto& name : component_names) {
        if (name == "prometheus") {
            components.emplace_back(
                std::make_unique<shield::metrics::PrometheusComponent>());
        } else if (name == "gateway") {
            // Find the LuaVMPool component
            shield::script::LuaVMPool* lua_vm_pool_ptr = nullptr;
            for (auto& comp : components) {
                if (comp->name() == "lua_vm_pool") {
                    lua_vm_pool_ptr =
                        static_cast<shield::script::LuaVMPool*>(comp.get());
                    break;
                }
            }
            if (!lua_vm_pool_ptr) {
                SHIELD_LOG_ERROR << "LuaVMPool component not found, cannot "
                                    "initialize GatewayComponent.";
                return 1;  // Exit if LuaVMPool is not available
            }
            components.emplace_back(
                std::make_unique<shield::gateway::GatewayComponent>(
                    "gateway", distributed_actor_system, *lua_vm_pool_ptr));
        } else if (name == "lua_vm_pool") {
            shield::script::LuaVMPoolConfig lua_vm_pool_config;

            // Load LuaVMPoolConfig from config file
            try {
                lua_vm_pool_config.initial_size =
                    config.get<size_t>("lua_vm_pool.initial_size");
                lua_vm_pool_config.min_size =
                    config.get<size_t>("lua_vm_pool.min_size");
                lua_vm_pool_config.max_size =
                    config.get<size_t>("lua_vm_pool.max_size");
                lua_vm_pool_config.idle_timeout = std::chrono::milliseconds(
                    config.get<int>("lua_vm_pool.idle_timeout_ms"));
                lua_vm_pool_config.acquire_timeout = std::chrono::milliseconds(
                    config.get<int>("lua_vm_pool.acquire_timeout_ms"));
                lua_vm_pool_config.preload_scripts =
                    config.get<bool>("lua_vm_pool.preload_scripts");

                SHIELD_LOG_INFO << "LuaVMPool configured: initial="
                                << lua_vm_pool_config.initial_size
                                << ", min=" << lua_vm_pool_config.min_size
                                << ", max=" << lua_vm_pool_config.max_size
                                << ", preload_scripts="
                                << lua_vm_pool_config.preload_scripts;
            } catch (const std::exception& e) {
                SHIELD_LOG_WARN << "Failed to load LuaVMPool configuration, "
                                   "using defaults: "
                                << e.what();
                // Keep default values from LuaVMPoolConfig constructor
            }

            components.emplace_back(std::make_unique<shield::script::LuaVMPool>(
                "lua_vm_pool", lua_vm_pool_config));
        } else if (name == "logger") {
            // Logger is already configured, but could be a component too
        }
        // Add other components here
    }

    // Initialize and start components
    for (auto& comp : components) {
        comp->init();
        comp->start();
    }

    SHIELD_LOG_INFO << "Application running. Press Ctrl+C to exit.";

    // Wait for termination signal
    while (g_signal_status == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    SHIELD_LOG_INFO << "Shutdown signal received. Stopping components...";

    // Stop components in reverse order
    for (auto it = components.rbegin(); it != components.rend(); ++it) {
        (*it)->stop();
    }

    SHIELD_LOG_INFO << "Application finished.";

    return 0;
}
