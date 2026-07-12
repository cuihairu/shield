// [SHIELD_BOOTSTRAP] Bootstrap implementation
#include "shield/bootstrap/bootstrap.hpp"

#include "shield/base/error.hpp"
#include "shield/bootstrap/starter.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/plugin_host.hpp"
#include "shield/plugin/protocol_codec.h"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <caf/actor_system.hpp>
#include <caf/io/all.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/core/caf_adapter.hpp"
#include "shield/lua/lua_gateway_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/net/console_server.hpp"
#include "shield/console/command_dispatcher.hpp"
#include "shield/console/root_commands.hpp"
#include "shield/console/lua_commands.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/listener.hpp"
#include "shield/shield.hpp"
#include "shield/transport/protocol.hpp"

namespace shield::bootstrap {
using shield::core::CafAdapter;
using shield::core::initialize_core;

namespace {

shield::log::Level parse_log_level(const std::string& value) {
    if (value == "debug") return shield::log::Level::Debug;
    if (value == "warn") return shield::log::Level::Warning;
    if (value == "error") return shield::log::Level::Error;
    return shield::log::Level::Info;
}

std::string resolve_script_path_with_lua_path(
    const shield::config::RuntimeActorConfig& actor) {
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

    auto lua_script_path = shield::config::get("lua.script_path", "scripts");
    auto from_lua_path = std::filesystem::path(lua_script_path) / script;
    if (std::filesystem::exists(from_lua_path)) {
        return from_lua_path.string();
    }

    return script.string();
}

std::string resolve_script_path(
    const shield::config::RuntimeActorConfig& actor) {
    return resolve_script_path_with_lua_path(actor);
}

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

std::optional<Endpoint> parse_endpoint(const std::string& value) {
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon + 1 >= value.size()) {
        return std::nullopt;
    }
    Endpoint endpoint;
    endpoint.host = value.substr(0, colon);
    try {
        const int port = std::stoi(value.substr(colon + 1));
        if (port < 1 || port > 65535) {
            return std::nullopt;
        }
        endpoint.port = static_cast<uint16_t>(port);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return endpoint;
}

shield::transport::ExternalBodyCodecResolver make_protocol_codec_resolver() {
    return [](std::string_view provider, std::string_view codec_name,
              std::string* error) -> const shield_protocol_codec_v1* {
        const auto provider_name = std::string(provider);
        const auto* codec =
            shield::plugin::global_host()
                .get_by_binding<shield_protocol_codec_v1>(provider_name);
        if (codec == nullptr) {
            if (error) {
                *error = "protocol codec provider '" + provider_name +
                         "' is not configured or does not provide " +
                         SHIELD_PROTOCOL_CODEC_INTERFACE;
            }
            return nullptr;
        }
        if (codec->codec_name == nullptr ||
            std::string_view(codec->codec_name) != codec_name) {
            if (error) {
                *error = "protocol codec provider '" + provider_name +
                         "' does not serve codec '" + std::string(codec_name) +
                         "'";
            }
            return nullptr;
        }
        if (codec->decode == nullptr || codec->encode == nullptr) {
            if (error) {
                *error = "protocol codec provider '" + provider_name +
                         "' has incomplete vtable";
            }
            return nullptr;
        }
        return codec;
    };
}

shield::transport::ProtocolBuildOptions protocol_build_options(
    std::string_view source_dir, std::size_t fallback_max_frame_size) {
    shield::transport::ProtocolBuildOptions options;
    options.source_dir = source_dir;
    options.fallback_max_frame_size = fallback_max_frame_size;
    options.external_codec_resolver = make_protocol_codec_resolver();
    return options;
}

}  // namespace

// Global state
struct GlobalState {
    RuntimeConfig config;
    std::unique_ptr<caf::actor_system> actor_system;
    std::unique_ptr<CafAdapter> caf_adapter;
    std::unique_ptr<shield::lua::LuaRuntime> lua_runtime;
    std::unique_ptr<shield::lua::LuaServiceManager> lua_services;
    boost::asio::io_context net_io;
    // A work guard keeps net_io.run() alive even when no async operations
    // are pending, preventing the thread pool from spinning down prematurely.
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>
        net_work_guard;
    std::vector<std::thread> net_threads;
    std::vector<std::unique_ptr<shield::lua::LuaGatewayBridge>> gateway_bridges;
    std::vector<std::unique_ptr<shield::net::TcpListener>> tcp_listeners;
    std::unique_ptr<shield::net::ConsoleServer> console_server;
    std::unique_ptr<shield::console::CommandDispatcher> console_dispatcher;
#ifdef SHIELD_ENABLE_CLUSTER
    std::unique_ptr<shield::cluster::ClusterManager> cluster_manager;
#endif
    bool initialized = false;
};

static GlobalState* g_state = nullptr;
static std::unique_ptr<GlobalState> g_state_owner;

void cleanup_failed_initialize() {
    if (g_state) {
        for (auto& listener : g_state->tcp_listeners) {
            if (listener) {
                listener->stop();
            }
        }
        g_state->net_work_guard.reset();
        g_state->net_io.stop();
        for (auto& t : g_state->net_threads) {
            if (t.joinable()) t.join();
        }
        g_state->tcp_listeners.clear();
        g_state->gateway_bridges.clear();
        if (g_state->console_server) {
            g_state->console_server->stop();
            g_state->console_server.reset();
            g_state->console_dispatcher.reset();
        }
        if (g_state->lua_services) {
            g_state->lua_services->shutdown_all("startup_failed");
        }
        g_state->lua_services.reset();
        g_state->lua_runtime.reset();
        shield::plugin::global_host().shutdown();
#ifdef SHIELD_ENABLE_CLUSTER
        if (g_state->cluster_manager) {
            g_state->cluster_manager->stop();
        }
        shield::cluster::set_global_cluster_manager(nullptr);
        g_state->cluster_manager.reset();
#endif
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
            cleanup_failed_initialize();
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

    shield::config::RuntimeValidationOptions validation_options;
#ifdef SHIELD_ENABLE_CLUSTER
    validation_options.cluster_enabled = true;
#else
    validation_options.cluster_enabled = false;
#endif
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

    // Start the plugin system. All runtime subsystems that used to resolve
    // bindings through host-side singletons (database/cache/etc.) now go
    // through PluginHost directly; the plugin pipeline owns startup ordering
    // and library lifetime.
    {
        auto plugin_cfg = shield::plugin::load_plugin_config();
        std::string plugin_err;
        if (!shield::plugin::global_host().startup(plugin_cfg, plugin_err)) {
            SHIELD_LOG_ERROR(log, "Plugin startup failed: " + plugin_err);
            cleanup_failed_initialize();
            return false;
        }
        SHIELD_LOG_INFO(log, "Plugin system started");
    }

#ifdef SHIELD_ENABLE_CLUSTER
    auto cluster_config = shield::cluster::parse_cluster_config();
    if (cluster_config.enabled) {
        if (cluster_config.node_id.empty()) {
            SHIELD_LOG_ERROR(
                log, "Invalid cluster config: cluster.node_id is required");
            cleanup_failed_initialize();
            return false;
        }
        g_state->cluster_manager =
            std::make_unique<shield::cluster::ClusterManager>(cluster_config);
        g_state->cluster_manager->start();
        shield::cluster::set_global_cluster_manager(
            g_state->cluster_manager.get());
    }
#endif

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
                {"config",
                 nlohmann::json::parse(actor.options_json, nullptr, false)},
            };
            if (opts["config"].is_discarded()) {
                opts["config"] = nlohmann::json::object();
            }

            auto result = g_state->lua_services->spawn(
                resolve_script_path(actor), opts.dump());
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

    for (const auto& actor : shield::config::runtime_actors()) {
        if (actor.network_tcp.empty() || actor.instances == 0) {
            continue;
        }
        auto endpoint = parse_endpoint(actor.network_tcp);
        if (!endpoint) {
            SHIELD_LOG_ERROR(log, "Invalid TCP endpoint for actor '" +
                                      actor.name + "': " + actor.network_tcp);
            cleanup_failed_initialize();
            return false;
        }
        if (endpoint->host != "0.0.0.0" && endpoint->host != "*" &&
            endpoint->host != "::" && endpoint->host != "localhost" &&
            endpoint->host != "127.0.0.1") {
            SHIELD_LOG_WARNING(
                log,
                "TcpListener currently binds all IPv4 interfaces; "
                "configured host is " +
                    endpoint->host);
        }
        if (actor.network_protocol_enabled) {
            std::string protocol_error;
            auto protocol_options =
                protocol_build_options(actor.source_dir, actor.max_frame_size);
            auto probe = shield::transport::build_protocol_pipeline_from_json(
                actor.network_protocol_json, protocol_options, &protocol_error);
            if (!probe) {
                SHIELD_LOG_ERROR(log, "Invalid TCP protocol for actor '" +
                                          actor.name + "': " + protocol_error);
                cleanup_failed_initialize();
                return false;
            }
        }

        auto bridge = std::make_unique<shield::lua::LuaGatewayBridge>(
            *g_state->lua_services, actor.name);
        shield::net::SessionCallbacks callbacks;
        callbacks.on_connect =
            [bridge_ptr =
                 bridge.get()](std::shared_ptr<shield::net::Session> session) {
                bridge_ptr->on_connect(std::move(session));
            };
        // on_message is removed - all messages go through protocol path
        // (on_packet). Raw bytes without protocol are rejected.
        callbacks.on_disconnect =
            [bridge_ptr = bridge.get()](
                std::shared_ptr<shield::net::Session> session,
                std::string_view reason) {
                bridge_ptr->on_disconnect(std::move(session),
                                          std::string(reason));
            };
        callbacks.on_packet =
            [bridge_ptr = bridge.get()](
                std::shared_ptr<shield::net::Session> session,
                const shield::transport::DispatchResult& packet) {
                bridge_ptr->on_packet(std::move(session), packet);
            };
        if (actor.network_protocol_enabled) {
            const auto protocol_json = actor.network_protocol_json;
            const auto source_dir = actor.source_dir;
            const auto listener_max_frame_size = actor.max_frame_size;
            callbacks.create_protocol_pipeline =
                [protocol_json, source_dir, listener_max_frame_size]() mutable {
                    std::string protocol_error;
                    auto protocol_options = protocol_build_options(
                        source_dir, listener_max_frame_size);
                    auto pipeline =
                        shield::transport::build_protocol_pipeline_from_json(
                            protocol_json, protocol_options, &protocol_error);
                    if (!pipeline && !protocol_error.empty()) {
                        auto& log = shield::log::get_logger("bootstrap");
                        SHIELD_LOG_ERROR(
                            log, "Invalid network protocol: " + protocol_error);
                    }
                    return pipeline;
                };
        }

        auto listener = std::make_unique<shield::net::TcpListener>(
            g_state->net_io, endpoint->port, std::move(callbacks));
        if (!listener->is_open()) {
            SHIELD_LOG_ERROR(log, "Failed to start TCP listener for actor '" +
                                      actor.name + "' on " + actor.network_tcp);
            cleanup_failed_initialize();
            return false;
        }
        if (actor.max_connections > 0) {
            listener->set_max_connections(actor.max_connections);
        }
        if (actor.max_connections_per_ip > 0) {
            listener->set_max_per_ip(actor.max_connections_per_ip);
        }
        if (actor.max_frame_size > 0) {
            listener->set_max_frame_size(actor.max_frame_size);
        }
        if (actor.max_session_send_queue > 0) {
            listener->set_max_send_queue(actor.max_session_send_queue);
        }
        if (actor.read_idle_timeout_ms > 0) {
            listener->set_read_idle_timeout(actor.read_idle_timeout_ms);
        }
        listener->start();
        SHIELD_LOG_INFO(log, "TCP gateway listener started for actor '" +
                                 actor.name + "' on " + actor.network_tcp);
        g_state->gateway_bridges.push_back(std::move(bridge));
        g_state->tcp_listeners.push_back(std::move(listener));
    }

    if (!g_state->tcp_listeners.empty()) {
        const size_t net_threads_count = shield::config::runtime_net_threads();
        if (net_threads_count > 0) {
            g_state->net_work_guard.emplace(g_state->net_io.get_executor());
            for (size_t i = 0; i < net_threads_count; ++i) {
                g_state->net_threads.emplace_back(
                    [io = &g_state->net_io]() { io->run(); });
            }
        } else {
            // Legacy single-threaded mode
            g_state->net_threads.emplace_back([]() { g_state->net_io.run(); });
        }
    }

    // Run POST_START starters
    run_starters(Phase::POST_START);

    // Start the Lua worker thread. After this point, all mailbox drains,
    // timer fires, coroutine timeouts, and forked tasks happen on the worker.
    if (g_state->lua_services) {
        g_state->lua_services->start_worker();
    }

    // Start console server if enabled
    if (shield::config::get("console.enabled", "false") == "true" &&
        g_state->lua_services && g_state->lua_runtime) {
        auto sock_path = shield::config::get(
            "console.socket_path", "/tmp/shield-console.sock");
        try {
            g_state->console_server =
                std::make_unique<shield::net::ConsoleServer>(g_state->net_io,
                                                             sock_path);
            g_state->console_dispatcher =
                std::make_unique<shield::console::CommandDispatcher>();

            // Register command handlers
            auto root_cmds = std::make_shared<shield::console::RootCommands>(
                *g_state->lua_services);
            root_cmds->register_all(*g_state->console_dispatcher);

            auto lua_cmds = std::make_shared<shield::console::LuaCommands>(
                *g_state->lua_services, *g_state->lua_runtime);
            lua_cmds->register_all(*g_state->console_dispatcher);

            // Wire the line handler from the dispatcher to the server
            auto& dispatcher = *g_state->console_dispatcher;
            g_state->console_server->set_on_line(
                [&dispatcher](std::shared_ptr<shield::net::ConsoleSession> session,
                              std::string line) {
                    dispatcher.dispatch(session, line);
                });

            g_state->console_server->start();
            SHIELD_LOG_INFO(log, "Console server listening on " + sock_path);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR(log,
                             std::string("Failed to start console server: ") +
                                 e.what());
        }
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

    // Stop console server before other components
    if (g_state->console_server) {
        g_state->console_server->stop();
        g_state->console_server.reset();
        g_state->console_dispatcher.reset();
    }

    // Stop the Lua worker first so no new Lua code runs while we tear down.
    for (auto& listener : g_state->tcp_listeners) {
        if (listener) {
            listener->stop();
        }
    }
    g_state->net_work_guard.reset();
    g_state->net_io.stop();
    for (auto& t : g_state->net_threads) {
        if (t.joinable()) t.join();
    }
    g_state->tcp_listeners.clear();
    g_state->gateway_bridges.clear();

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
#ifdef SHIELD_ENABLE_CLUSTER
    if (g_state->cluster_manager) {
        g_state->cluster_manager->stop();
    }
    shield::cluster::set_global_cluster_manager(nullptr);
    g_state->cluster_manager.reset();
#endif

    // Run POST_SHUTDOWN starters
    run_starters(Phase::POST_SHUTDOWN);

    // Tear down the plugin system (invokes each instance's shutdown
    // callback). Libraries stay mapped until process exit so any holder of a
    // resolved vtable remains valid.
    shield::plugin::global_host().shutdown();

    g_state->initialized = false;
    SHIELD_LOG_INFO(log, "Shield runtime shutdown complete");

    // Shutdown logging after the final runtime log has been emitted.
    shield::log::Logger::shutdown();

    g_state_owner.reset();
    g_state = nullptr;
}

// Check if initialized
bool is_initialized() { return g_state && g_state->initialized; }

int run(int argc, char** argv) { return shield::run(argc, argv); }

}  // namespace shield::bootstrap
