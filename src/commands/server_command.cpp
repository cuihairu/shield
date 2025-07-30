#include "shield/commands/server_command.hpp"

#include <unistd.h>  // for gethostname(), getpid()

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/fs/file_watcher.hpp"  // Updated include path for FileWatcher
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/logger.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/version.hpp"

// Initialize CAF type system BEFORE including other headers
#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/init_global_meta_objects.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/actor/lua_actor.hpp"
#include "shield/caf_type_ids.hpp"
#include "shield/core/component.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/gateway/gateway_component.hpp"
#include "shield/metrics/prometheus_component.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/serialization/universal_serialization_system.hpp"

// Global flag for signal handling
extern volatile sig_atomic_t g_signal_status;
extern void signal_handler(int signal);

namespace shield::commands {

ServerCommand::ServerCommand()
    : shield::cli::Command("server", "Start the Shield game server") {
    setup_flags();
    set_long_description(
        "Start the Shield game server with the specified configuration. "
        "This is the main server process that handles game clients.")
        .set_usage("shield server [OPTIONS]")
        .set_example(
            "  shield server --config config/prod.yaml --port 8080\\n"
            "  shield server --daemon --host 0.0.0.0");
}

void ServerCommand::setup_flags() {
    add_flag("config", "Configuration file path",
             shield::config::ConfigPaths::DEFAULT_CONFIG_FILE);
    add_int_flag("port", "Server port", 0);
    add_flag("host", "Server host", "");
    add_bool_flag("daemon", "Run as daemon", false);
}

int ServerCommand::run(shield::cli::CommandContext& ctx) {
    std::cout << "Starting Shield Server..." << std::endl;

    // Load configuration using new ConfigManager
    auto& config_manager = shield::config::ConfigManager::instance();
    try {
        std::string config_file = ctx.get_flag("config");
        config_manager.load_config(config_file,
                                   shield::config::ConfigFormat::YAML);
        std::cout << "Loaded configuration from: " << config_file << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load configuration: " << e.what() << std::endl;
        return 1;
    }

    // Initialize logging
    shield::log::LogConfig log_config;
    try {
        log_config.global_level =
            shield::log::Logger::level_from_string("info");
    } catch (...) {
        // Use default level if config not found
    }
    shield::log::Logger::init(log_config);

    // Start FileWatcher for dynamic config reloading
    auto config_watcher = std::make_unique<shield::fs::FileWatcher>();
    std::string config_file = ctx.get_flag("config");
    config_watcher->add_file(config_file);
    config_watcher->add_handler(
        [&config_manager, config_file](const shield::fs::FileEvent& event) {
            if (event.event_type == shield::fs::FileEventType::Modified) {
                try {
                    config_manager.reload_config(
                        config_file, shield::config::ConfigFormat::YAML);
                    SHIELD_LOG_INFO << "Configuration reloaded successfully";
                } catch (const std::exception& e) {
                    SHIELD_LOG_ERROR << "Failed to reload configuration: "
                                     << e.what();
                }
            }
        });
    config_watcher->start();

    // Show banner if configured
    bool show_startup_banner = true;
    try {
        const auto& config_tree = config_manager.get_config_tree();
        show_startup_banner = config_tree.get<bool>("app.show_banner", true);
    } catch (...) {
        // Use default
    }

    if (show_startup_banner) {
        shield::print_version();
    }

    // Set up signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Apply command line overrides
    if (ctx.has_flag("port") && ctx.get_int_flag("port") > 0) {
        std::cout << "Overriding port to: " << ctx.get_int_flag("port")
                  << std::endl;
        // TODO: Override config
    }

    if (ctx.has_flag("host") && !ctx.get_flag("host").empty()) {
        std::cout << "Overriding host to: " << ctx.get_flag("host")
                  << std::endl;
        // TODO: Override config
    }

    if (ctx.get_bool_flag("daemon")) {
        std::cout << "Running in daemon mode..." << std::endl;
        // TODO: Implement daemon mode
    }

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
    std::string node_id_config = "auto";  // default value
    try {
        const auto& config_tree = config_manager.get_config_tree();
        node_id_config =
            config_tree.get<std::string>("server.actor_system.node_id", "auto");
    } catch (...) {
        // Use default
    }
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
    std::vector<std::string> component_names;
    try {
        const auto& config_tree = config_manager.get_config_tree();
        // Try to get components list from config, fall back to defaults
        if (auto components_node =
                config_tree.get_child_optional("server.components")) {
            for (const auto& component : *components_node) {
                component_names.push_back(
                    component.second.get_value<std::string>());
            }
        } else {
            // Default components
            component_names = {"gateway", "prometheus", "lua_vm_pool"};
        }
    } catch (...) {
        // Use default components
        component_names = {"gateway", "prometheus", "lua_vm_pool"};
    }

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

            // Get Gateway configuration
            auto gateway_config =
                shield::config::ConfigManager::instance()
                    .get_component_config<shield::gateway::GatewayConfig>();
            if (!gateway_config) {
                SHIELD_LOG_ERROR << "Gateway configuration not found";
                return 1;
            }

            components.emplace_back(
                std::make_unique<shield::gateway::GatewayComponent>(
                    "gateway", distributed_actor_system, *lua_vm_pool_ptr,
                    gateway_config));
        } else if (name == "lua_vm_pool") {
            shield::script::LuaVMPoolConfig lua_vm_pool_config;

            // Load LuaVMPoolConfig from config file
            try {
                const auto& config_tree = config_manager.get_config_tree();
                lua_vm_pool_config.initial_size =
                    config_tree.get<size_t>("lua_vm_pool.initial_size", 5);
                lua_vm_pool_config.min_size =
                    config_tree.get<size_t>("lua_vm_pool.min_size", 2);
                lua_vm_pool_config.max_size =
                    config_tree.get<size_t>("lua_vm_pool.max_size", 20);
                lua_vm_pool_config.idle_timeout = std::chrono::milliseconds(
                    config_tree.get<int>("lua_vm_pool.idle_timeout_ms", 30000));
                lua_vm_pool_config.acquire_timeout =
                    std::chrono::milliseconds(config_tree.get<int>(
                        "lua_vm_pool.acquire_timeout_ms", 5000));
                lua_vm_pool_config.preload_scripts =
                    config_tree.get<bool>("lua_vm_pool.preload_scripts", false);

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

    // Stop FileWatcher
    config_watcher->stop();

    SHIELD_LOG_INFO << "Application finished.";

    return 0;
}

}  // namespace shield::commands