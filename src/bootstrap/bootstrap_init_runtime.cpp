// [SHIELD_BOOTSTRAP] Bootstrap phases: config/logging/crashpad setup
// and validation, POST_CONFIG starters, plugin-system startup and the
// cluster/player/server/global module initializers.
// Split out of bootstrap.cpp.
#include "shield/base/error.hpp"
#include "shield/bootstrap/bootstrap.hpp"
#include "shield/bootstrap/starter.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

#ifdef SHIELD_ENABLE_CRASHPAD
#include "shield/crash/crash.hpp"
#endif
#include "shield/plugin/plugin_host.hpp"
#include "shield/plugin/protocol_codec.h"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#include "shield/cluster/node_discovery.hpp"
#endif
#ifdef SHIELD_ENABLE_PLAYER
#include "shield/player/player_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_SERVER
#include "shield/server/server_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_GLOBAL
#include "shield/global/global_manager.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <caf/actor_system.hpp>
#include <caf/io/all.hpp>
#include <caf/scoped_actor.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bootstrap_impl.hpp"
#include "shield/caf_initializer.hpp"
#include "shield/console/command_dispatcher.hpp"
#include "shield/console/lua_commands.hpp"
#include "shield/console/ops_http_handler.hpp"
#include "shield/console/root_commands.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_gateway_bridge.hpp"
#include "shield/lua/lua_http_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/console_server.hpp"
#include "shield/net/listener.hpp"
#include "shield/net/tls_context.hpp"
#include "shield/shield.hpp"
#include "shield/transport/protocol.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::bootstrap {

namespace {

shield::log::Level parse_log_level(const std::string& value) {
    if (value == "debug") return shield::log::Level::Debug;

    if (value == "warn") return shield::log::Level::Warning;

    if (value == "error") return shield::log::Level::Error;

    return shield::log::Level::Info;
}

}  // namespace

bool init_config_and_logging(const RuntimeConfig& config) {
    // Initialize logging

    shield::log::Logger::initialize();

    shield::log::Logger::set_global_level(parse_log_level(config.log_level));

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

            return false;
        }
    }

    if (!config.node_id.empty()) {
        shield::config::global_config().set("cluster.node_id", config.node_id);
    }

    const auto configured_log_level =

        shield::config::get("log.level", config.log_level);

    shield::log::Logger::set_global_level(

        parse_log_level(configured_log_level));

    // Apply log.console / log.file.* sink configuration now that the config

    // is loaded. Logger::initialize() installed a console sink before this

    // point; apply_sinks rebuilds the list from the effective configuration.

    {
        const bool log_console =

            shield::config::get("log.console", "true") == "true";

        const bool log_file =

            shield::config::get("log.file.enabled", "false") == "true";

        const auto file_path =

            shield::config::get("log.file.path", "logs/shield.log");

        const auto max_size_mb =

            shield::config::get_int("log.file.max_size_mb", 100);

        const auto max_files =

            shield::config::get_int("log.file.max_files", 10);

        shield::log::Logger::apply_sinks(

            log_console, log_file, file_path,

            static_cast<size_t>(std::max<int64_t>(1, max_size_mb)) * 1024 *

                1024,

            static_cast<int>(std::max<int64_t>(1, max_files)));

        if (log_file) {
            auto& log = shield::log::get_logger("bootstrap");

            SHIELD_LOG_INFO(log, "File logging enabled: " + file_path);
        }
    }

#ifdef SHIELD_ENABLE_CRASHPAD

    // Crash collector (docs/crash-reporting.md): the earliest point where

    // both the config files and the logger are ready, before validation

    // and subsystem starters, so the rest of bootstrap runs under its

    // protection. Degrades to an ERROR log, never blocks startup.

    shield::crash::initialize();

#endif

    shield::config::RuntimeValidationOptions validation_options;

#ifdef SHIELD_ENABLE_CLUSTER

    validation_options.cluster_enabled = true;

#else

    validation_options.cluster_enabled = false;

#endif

#ifdef SHIELD_ENABLE_GLOBAL

    validation_options.global_enabled = true;

#else

    validation_options.global_enabled = false;

#endif

#ifdef SHIELD_ENABLE_PLAYER

    validation_options.player_enabled = true;

#else

    validation_options.player_enabled = false;

#endif

#ifdef SHIELD_ENABLE_SERVER

    validation_options.server_enabled = true;

#else

    validation_options.server_enabled = false;

#endif

    validation_options.ops_enabled = false;

    std::string validation_error;

    if (!shield::config::validate_runtime_config(validation_options,

                                                 &validation_error)) {
        SHIELD_LOG_ERROR(log, "Invalid config: " + validation_error);

        return false;
    }

    return true;
}

bool start_plugins() {
    auto& log = shield::log::get_logger("bootstrap");

    // Start the plugin system. All runtime subsystems that used to resolve

    // bindings through host-side singletons (database/cache/etc.) now go

    // through PluginHost directly; the plugin pipeline owns startup ordering

    // and library lifetime.

    {
        auto plugin_cfg = shield::plugin::load_plugin_config();

        std::string plugin_err;

        if (!shield::plugin::global_host().startup(plugin_cfg, plugin_err)) {
            SHIELD_LOG_ERROR(log, "Plugin startup failed: " + plugin_err);

            return false;
        }

        SHIELD_LOG_INFO(log, "Plugin system started");
    }

    return true;
}

#ifdef SHIELD_ENABLE_CLUSTER

bool init_cluster_manager(shield::cluster::ClusterConfig* out) {
    auto& log = shield::log::get_logger("bootstrap");

    shield::cluster::ClusterConfig cluster_config;

    try {
        cluster_config = shield::cluster::parse_cluster_config();

    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR(log,

                         std::string("Invalid cluster config: ") + e.what());

        return false;
    }

    if (cluster_config.enabled) {
        if (cluster_config.node_id.empty()) {
            SHIELD_LOG_ERROR(

                log, "Invalid cluster config: cluster.node_id is required");

            return false;
        }

        g_state->cluster_manager =

            std::make_unique<shield::cluster::ClusterManager>(cluster_config);

        // Optional discovery (cluster.discovery.type): attach before start

        // so the first registration/scan rides the normal startup path.

        // A null return (type "" or unknown) keeps static-peer-only mode.

        if (auto discovery = shield::cluster::make_node_discovery(

                cluster_config.discovery, cluster_config.node_id,

                cluster_config.listen_address)) {
            g_state->cluster_manager->set_node_discovery(std::move(discovery));
        }

        g_state->cluster_manager->start();

        shield::cluster::set_global_cluster_manager(

            g_state->cluster_manager.get());
    }

    *out = cluster_config;

    return true;
}

#endif

#ifdef SHIELD_ENABLE_PLAYER

bool init_player_module() {
    auto& log = shield::log::get_logger("bootstrap");

    // Player module: parse its own config section up front (fail fast on a

    // malformed `player:` block) and install the process-wide session index.

    // The manager always exists when the module is compiled in; the Lua-side

    // setup() is what turns it on for business use.

    {
        shield::player::PlayerConfig player_config;

        std::string player_error;

        // from_global_config reads only defaulted keys and always succeeds

        // (it never reports an error), so the `||` short-circuit arc is dead.

        if (!shield::player::PlayerConfig::from_global_config(

                &player_config,

                &player_error) ||  // GCOVR_EXCL_BR_LINE (from_global_config

            // never fails)

            !shield::player::validate_player_config(  // GCOVR_EXCL_BR_LINE

                player_config,  // (compiler artifact: call-dispatch clone

                // arcs; the false return is driven by the

                // invalid-player-config test)

                &player_error)) {
            SHIELD_LOG_ERROR(log, "Invalid player config: " + player_error);

            return false;
        }

        g_state->player_manager =

            std::make_unique<shield::player::PlayerManager>(player_config);

#ifdef SHIELD_ENABLE_CLUSTER

        if (g_state->cluster_manager) {
            g_state->player_manager->set_locality(

                g_state->cluster_manager->node_id(),

                g_state->cluster_manager->node_epoch());
        }

#endif

        shield::player::PlayerManager::set_global(

            g_state->player_manager.get());

        SHIELD_LOG_INFO(log, "Player subsystem initialized");
    }

    return true;
}

#endif

#ifdef SHIELD_ENABLE_SERVER

bool init_server_module() {
    auto& log = shield::log::get_logger("bootstrap");

    // Server module: parse its own config section up front (fail fast on a

    // malformed `server_manager:` block) and install the process-wide state

    // machine. State truth is C++-side; the Lua facade only reads and

    // migrates it. mark_ready() below flips the state machine to `running`

    // once initialization completes.

    {
        shield::server::ServerConfig server_config;

        std::string server_error;

        // from_global_config reads only defaulted keys and always succeeds

        // (it never reports an error), so the `||` short-circuit arc is dead.

        if (!shield::server::ServerConfig::from_global_config(

                &server_config,

                &server_error) ||  // GCOVR_EXCL_BR_LINE (from_global_config

            // never fails)

            !shield::server::validate_server_config(server_config,

                                                    &server_error)) {
            SHIELD_LOG_ERROR(log, "Invalid server config: " + server_error);

            return false;
        }

        g_state->server_manager =

            std::make_unique<shield::server::ServerManager>(server_config);

#ifdef SHIELD_ENABLE_CLUSTER

        if (g_state->cluster_manager) {
            g_state->server_manager->set_locality(

                g_state->cluster_manager->node_id());
        }

#endif

        g_state->server_manager->set_version_fallback(

            shield::config::get("app.version", ""));

        shield::server::ServerManager::set_global(

            g_state->server_manager.get());

        SHIELD_LOG_INFO(log, "Server subsystem initialized");
    }

    return true;
}

#endif

#ifdef SHIELD_ENABLE_GLOBAL

bool init_global_module() {
    auto& log = shield::log::get_logger("bootstrap");

    // Global module: parse its own config section up front (fail fast on

    // an invalid `global:` block) and install the process-wide store.

    // Scheduler task delivery is wired after the Lua services exist; the

    // tick thread starts there too.

    {
        shield::global::GlobalConfig global_config;

        std::string global_error;

        // Same dead short-circuit as the server block above: the global config

        // parse reads defaulted ints only and cannot fail.

        if (!shield::global::GlobalConfig::from_global_config(

                &global_config,

                &global_error) ||  // GCOVR_EXCL_BR_LINE (from_global_config

            // never fails)

            !shield::global::validate_global_config(global_config,

                                                    &global_error)) {
            SHIELD_LOG_ERROR(log, "Invalid global config: " + global_error);

            return false;
        }

        g_state->global_manager =

            std::make_unique<shield::global::GlobalManager>(global_config);

        shield::global::GlobalManager::set_global(

            g_state->global_manager.get());

        SHIELD_LOG_INFO(log, "Global subsystem initialized");
    }

    return true;
}

#endif

}  // namespace shield::bootstrap
