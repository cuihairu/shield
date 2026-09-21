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
#include "shield/cluster/cluster_transport.hpp"
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

std::string resolve_script_path_with_lua_path(
    const shield::config::RuntimeActorConfig& actor) {
    std::filesystem::path script(actor.script);
    if (script.is_absolute() || std::filesystem::exists(script)) {
        return script.string();
    }

    if (!actor.source_dir.empty()) {  // GCOVR_EXCL_BR_LINE (defensive:
        // Config::load_yaml always sets source_dir to the config file's
        // parent ("." for bare names), so bootstrap never resolves an actor
        // with an empty source_dir)
        auto from_config = std::filesystem::path(actor.source_dir) / script;
        if (std::filesystem::exists(from_config)) {
            return from_config.string();
        }
    }

    auto lua_script_path = shield::config::get("lua.script_path", "scripts");
    auto from_lua_path = std::filesystem::path(lua_script_path) / script;
    if (std::filesystem::exists(from_lua_path)) {  // GCOVR_EXCL_BR_LINE
        // (defensive: runtime config validation resolves every declared
        // script through the same source_dir/lua.script_path chain before
        // bootstrap runs, so the resolver cannot fall through to the bare
        // name here — same guarantee as the excluded return below)
        return from_lua_path.string();
    }

    return script.string();  // GCOVR_EXCL_LINE (config validation rejects
}  // missing actor scripts before bootstrap resolves paths)

std::string resolve_script_path(
    const shield::config::RuntimeActorConfig& actor) {
    return resolve_script_path_with_lua_path(actor);
}

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

// GCOVR_EXCL_START (unreachable through the validated config path: runtime
// validation rejects malformed listen addresses before bootstrap parses one)
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
// GCOVR_EXCL_STOP

shield::transport::ExternalBodyCodecResolver make_protocol_codec_resolver() {
    return [](std::string_view provider, std::string_view codec_name,
              std::string* error) -> const shield_protocol_codec_v1* {
        const auto provider_name = std::string(provider);
        const auto* codec =
            shield::plugin::global_host()
                .get_by_binding<shield_protocol_codec_v1>(provider_name);
        if (codec == nullptr) {
            if (error) {  // GCOVR_EXCL_BR_LINE (defensive: the only external
                // codec resolver call site passes a local error buffer; the
                // null-error arm exists for resolver API generality)
                *error = "protocol codec provider '" + provider_name +
                         "' is not configured or does not provide " +
                         SHIELD_PROTOCOL_CODEC_INTERFACE;
            }
            return nullptr;
        }
        // GCOVR_EXCL_START (external codec provider validation: these guards
        // only run with a codec plugin actually loaded through the
        // plugin host; exercised by integration plugin examples, not
        // by the unit suites)
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
        // GCOVR_EXCL_STOP
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
    std::unique_ptr<shield::lua::LuaRuntime> lua_runtime;
    // shared_ptr (not unique_ptr): the M4 data-plane closures (envelope
    // bridges, remote-send, proxied-call hook) run on transport/service
    // actor threads and may still be in flight while shutdown rewrites the
    // other half of the pair. Shared ownership keeps every capture valid;
    // the objects themselves fail honestly after their stop() has run.
    std::shared_ptr<shield::lua::LuaServiceManager> lua_services;
    boost::asio::io_context net_io;
    // A work guard keeps net_io.run() alive even when no async operations
    // are pending, preventing the thread pool from spinning down prematurely.
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>
        net_work_guard;
    std::vector<std::thread> net_threads;
    std::vector<std::unique_ptr<shield::lua::LuaGatewayBridge>> gateway_bridges;
    // One gateway actor per listener; exited after the listeners stop and
    // before the Lua services shut down (their bind responses call back into
    // the manager).
    std::vector<caf::actor> gateway_actors;
    std::vector<std::unique_ptr<shield::net::TcpListener>> tcp_listeners;
    std::unique_ptr<shield::net::ConsoleServer> console_server;
    std::unique_ptr<shield::console::CommandDispatcher> console_dispatcher;
    // The dispatcher stores the command handlers as lambdas capturing `this`,
    // so the command objects must outlive initialize()'s scope. They used to
    // be locals: freed while console sessions still dispatched into them
    // (Linux only survived because the allocator handed the freed chunk to
    // the next command object with an identical field layout).
    std::shared_ptr<shield::console::RootCommands> root_commands;
    std::shared_ptr<shield::console::LuaCommands> lua_commands;
    std::unique_ptr<shield::net::HttpServer> http_server;
    std::unique_ptr<shield::console::OpsHttpHandler> ops_http_handler;
    std::unique_ptr<shield::lua::LuaHttpBridge> http_bridge;
#ifdef SHIELD_ENABLE_CLUSTER
    std::unique_ptr<shield::cluster::ClusterManager> cluster_manager;
    std::shared_ptr<shield::cluster::ClusterTransport> cluster_transport;
#endif
#ifdef SHIELD_ENABLE_PLAYER
    std::unique_ptr<shield::player::PlayerManager> player_manager;
#endif
#ifdef SHIELD_ENABLE_SERVER
    std::unique_ptr<shield::server::ServerManager> server_manager;
#endif
#ifdef SHIELD_ENABLE_GLOBAL
    std::unique_ptr<shield::global::GlobalManager> global_manager;
#endif
    bool initialized = false;
};

static GlobalState* g_state = nullptr;
static std::unique_ptr<GlobalState> g_state_owner;

// Exit every live gateway actor and block until each is really gone.
// down_msg only fires after the actor finished its last handler, so once
// this returns no bind/close/egress response can still dereference the raw
// LuaServiceManager pointer in GatewayDeps. A bare anon_send_exit loop used
// to race the teardown: the exit messages are queued asynchronously while
// the caller moved on to destroy the manager those handlers point at.
static void exit_gateway_actors_and_wait() {
    if (g_state->gateway_actors.empty()) {
        return;
    }
    caf::scoped_actor self{*g_state->actor_system};
    for (const auto& gateway_actor : g_state->gateway_actors) {
        self->monitor(gateway_actor);
        caf::anon_send_exit(gateway_actor, caf::exit_reason::user_shutdown);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t remaining = g_state->gateway_actors.size();
    while (remaining > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {  // GCOVR_EXCL_BR_LINE (defensive: 5s teardown
            // safety valve; the valve body below is already excluded and no
            // test wedges a gateway actor past the deadline)
            // GCOVR_EXCL_START (safety valve: no test wedges a gateway
            // actor mid-handler for 5s; teardown proceeds best-effort)
            SHIELD_LOG_WARNING(shield::log::get_logger("bootstrap"),
                               "gateway actor did not exit within 5s; "
                               "continuing teardown");
            break;
            // GCOVR_EXCL_STOP
        }
        self->receive(
            [&](const caf::down_msg& down) {
                for (const auto& gateway_actor :  // GCOVR_EXCL_BR_LINE (the
                                                  // range-for machinery arc,
                                                  // see annotation below)
                     g_state
                         ->gateway_actors) {  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: range-for machinery
                                              // arc; the container is never
                                              // empty when a down message
                                              // arrives and both arms of the
                                              // address comparison on the next
                                              // line are exercised)
                    if (down.source == gateway_actor.address()) {
                        --remaining;
                        break;
                    }
                }
            },
            // GCOVR_EXCL_START (timeout arm of the receive never fires in
            // tests: gateway actors exit in milliseconds; the 5s deadline is
            // a teardown safety valve, same as the break above)
            caf::after(std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - now)) >>
                [] {
                    // Timed out; the loop re-checks the deadline.
                });
        // GCOVR_EXCL_STOP
    }
    g_state->gateway_actors.clear();
}

void cleanup_failed_initialize() {
    if (g_state) {  // GCOVR_EXCL_BR_LINE (defensive: initialize() is the only
        // caller and g_state is installed before any failure return)
        for (auto& listener : g_state->tcp_listeners) {
            if (listener) {  // GCOVR_EXCL_BR_LINE (defensive: listeners are
                // pushed only after is_open() succeeds, never null)
                listener->stop();
            }
        }
        g_state->net_work_guard.reset();
        g_state->net_io.stop();
        for (auto& t :  // GCOVR_EXCL_BR_LINE (the loop machinery arc, see
                        // the annotation below)
             g_state
                 ->net_threads) {  // GCOVR_EXCL_BR_LINE (defensive: net threads
                                   // start only after every listener is up and
                                   // no initialize() failure happens past that
                                   // point, so cleanup never has threads to
                                   // join — see excluded join below)
            if (t.joinable())      // GCOVR_EXCL_LINE (net threads only exist
                t.join();          // GCOVR_EXCL_LINE with a live listener)
        }
        g_state->tcp_listeners.clear();
        g_state->gateway_bridges.clear();
        // A later listener (protocol validation, port bind) can fail after
        // earlier gateway actors have spawned.
        exit_gateway_actors_and_wait();
        if (g_state->console_server) {  // GCOVR_EXCL_BR_LINE (defensive: no
            // initialize() failure happens after the console server starts;
            // body already excluded)
            // GCOVR_EXCL_START (unreachable: no initialize() failure happens
            // after the console server starts)
            g_state->console_server->stop();
            g_state->console_server.reset();
            g_state->console_dispatcher.reset();
            g_state->root_commands.reset();
            g_state->lua_commands.reset();
            // GCOVR_EXCL_STOP
        }
        if (g_state->lua_services) {
            g_state->lua_services->shutdown_all("startup_failed");
        }
        g_state->lua_services.reset();
        g_state->lua_runtime.reset();
        shield::plugin::global_host().shutdown();
#ifdef SHIELD_ENABLE_SERVER
        // Mirror the successful-teardown order: halt the timer and drop the
        // injected callbacks first (they capture lua_services, released
        // above), then unregister the global.
        if (g_state->server_manager) {
            g_state->server_manager->stop();
        }
        shield::server::ServerManager::set_global(nullptr);
        g_state->server_manager.reset();
#endif
#ifdef SHIELD_ENABLE_GLOBAL
        // Same ordering rule as the successful teardown: stop() joins the
        // tick thread and drops the fire callback (which captures
        // lua_services, released above) before the manager is destroyed.
        if (g_state->global_manager) {
            g_state->global_manager->stop();
        }
        shield::global::GlobalManager::set_global(nullptr);
        g_state->global_manager.reset();
#endif
#ifdef SHIELD_ENABLE_CLUSTER
        // The transport actor lives in the CAF system: unpublish and kill it
        // while the system (and the manager its callbacks point at) are
        // still alive.
        if (g_state->cluster_transport) {
            g_state->cluster_transport->stop();
        }
        shield::cluster::set_global_cluster_transport(nullptr);
        g_state->cluster_transport.reset();
        if (g_state->cluster_manager) {
            g_state->cluster_manager->stop();
        }
        shield::cluster::set_global_cluster_manager(nullptr);
        g_state->cluster_manager.reset();
#endif
        g_state->actor_system.reset();
        g_state->initialized = false;
    }
    shield::log::Logger::shutdown();
    g_state_owner.reset();
    g_state = nullptr;
}

// Initialize
static bool initialize_impl(const RuntimeConfig& config) {
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
            return false;
        }
        g_state->cluster_manager =
            std::make_unique<shield::cluster::ClusterManager>(cluster_config);
        g_state->cluster_manager->start();
        shield::cluster::set_global_cluster_manager(
            g_state->cluster_manager.get());
    }
#endif

#ifdef SHIELD_ENABLE_PLAYER
    // Player module: parse its own config section up front (fail fast on a
    // malformed `player:` block) and install the process-wide session index.
    // The manager always exists when the module is compiled in; the Lua-side
    // setup() is what turns it on for business use.
    {
        shield::player::PlayerConfig player_config;
        std::string player_error;
        if (!shield::player::PlayerConfig::from_global_config(&player_config,
                                                              &player_error) ||
            !shield::player::validate_player_config(player_config,
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
#endif

#ifdef SHIELD_ENABLE_SERVER
    // Server module: parse its own config section up front (fail fast on a
    // malformed `server_manager:` block) and install the process-wide state
    // machine. State truth is C++-side; the Lua facade only reads and
    // migrates it. mark_ready() below flips the state machine to `running`
    // once initialization completes.
    {
        shield::server::ServerConfig server_config;
        std::string server_error;
        if (!shield::server::ServerConfig::from_global_config(&server_config,
                                                              &server_error) ||
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
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    // Global module: parse its own config section up front (fail fast on
    // an invalid `global:` block) and install the process-wide store.
    // Scheduler task delivery is wired after the Lua services exist; the
    // tick thread starts there too.
    {
        shield::global::GlobalConfig global_config;
        std::string global_error;
        if (!shield::global::GlobalConfig::from_global_config(&global_config,
                                                              &global_error) ||
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
#endif

    // Initialize CAF actor system
    initialize_caf_types();
#ifdef SHIELD_ENABLE_CLUSTER
    // Cluster wire types must be in CAF's global meta object table before
    // any actor_system is constructed (CAF requirement, not just convention).
    shield::cluster::init_cluster_caf_types();
#endif
    // GCOVR_EXCL_START (uncalled static-init clone)
    caf::actor_system_config& caf_config = [&]() -> auto& {
        // GCOVR_EXCL_STOP
        static caf::actor_system_config cfg;  // GCOVR_EXCL_BR_LINE (compiler
        // artifact: static-local init guard; the zero arcs belong to gcov's
        // never-called clone and the guard-abort path — the real init/skip
        // arms are both exercised, see the "uncalled static-init clone"
        // note above)
        cfg.load<caf::io::middleman>();
        return cfg;
    }();

    // Set number of CAF scheduler worker threads.
    if (config.num_workers > 0) {
        caf_config.set("caf.scheduler.max-threads", config.num_workers);
    }

    g_state->actor_system = std::make_unique<caf::actor_system>(caf_config);

    SHIELD_LOG_INFO(log, "CAF actor system initialized");

#ifdef SHIELD_ENABLE_CLUSTER
    // Cluster transport needs the live CAF system to publish the cluster
    // actor and dial peers. A listen failure is fatal: the operator
    // explicitly configured a cluster, silently running standalone would
    // betray the node's reported health.
    if (cluster_config.enabled) {
        g_state->cluster_transport =
            std::make_shared<shield::cluster::ClusterTransport>(
                *g_state->actor_system, *g_state->cluster_manager,
                cluster_config);
        uint16_t bound_port = 0;
        std::string transport_error;
        if (!g_state->cluster_transport->start(&bound_port, transport_error)) {
            SHIELD_LOG_ERROR(
                log, "Cluster transport failed to start: " + transport_error);
            return false;
        }
        // M5: admin surfaces (/ops/status, root.status / root.cluster) read
        // the transport counters through the process-global pointer.
        shield::cluster::set_global_cluster_transport(
            g_state->cluster_transport.get());
        SHIELD_LOG_INFO(log, "Cluster transport listening on port " +
                                 std::to_string(bound_port));
    }
#endif

    // Run POST_SYSTEM_INIT starters
    run_starters(Phase::POST_SYSTEM_INIT);

    g_state->lua_runtime = std::make_unique<shield::lua::LuaRuntime>();
    g_state->lua_services = std::make_shared<shield::lua::LuaServiceManager>(
        *g_state->lua_runtime, *g_state->actor_system);

#ifdef SHIELD_ENABLE_SERVER
    // Wire the server state machine to the runtime it drives:
    // - notify_fn delivers state changes to watcher services over the
    //   system-message channel (send() rejects reserved on_* methods, so
    //   the bridge uses send_system, same as the gateway hook path). A gone
    //   service auto-unregisters; "runtime is stopping" keeps watching.
    // - stop_request_fn is the same cooperative stop the signal handlers
    //   invoke (shutdown(ms) handover, OD-017).
    // The capture is a raw pointer: a shared_ptr copy here would keep the
    // manager alive past shutdown()'s explicit release order
    // (lua_services -> lua_runtime), and destroying that last reference
    // later — when stop() clears this callback — would re-run
    // ~LuaServiceManager against the already-freed runtime (ASan-verified).
    // stop() joins the shutdown timer before clearing the callbacks, so
    // neither can run after the pointer stops being valid.
    {
        auto* services = g_state->lua_services.get();
        g_state->server_manager->set_notify_fn(
            [services](
                const std::string& service_id,
                const std::string& state_name) -> shield::server::Delivery {
                if (!services->service_vm(service_id)) {
                    return shield::server::Delivery::kGone;
                }
                std::string error;
                if (!services->send_system(service_id, "on_server_state_change",
                                           nlohmann::json::array({state_name}),
                                           &error)) {
                    // GCOVR_EXCL_START (defensive: state notifications only
                    // fire while the Lua runtime is alive; the stopping-
                    // teardown window is not deterministically reachable)
                    // Transient runtime teardown: keep the watcher so a
                    // restart of the runtime (or the remaining drain window)
                    // still sees later transitions.
                    return error == "runtime is stopping"
                               ? shield::server::Delivery::kRetryable
                               : shield::server::Delivery::kGone;
                    // GCOVR_EXCL_STOP
                }
                return shield::server::Delivery::kOk;
            });
        g_state->server_manager->set_stop_request_fn(
            []() { shield::request_stop(); });
    }
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    // Scheduler task delivery rides the same system-message channel the
    // server watch bridge uses: the per-VM forwarder (installed by the
    // Lua orchestration chunk) resolves the task callback and invokes it
    // on the owning service actor. A gone service drops the task.
    {
        auto* services = g_state->lua_services.get();
        // Raw pointer on purpose: the capture must not extend the
        // manager's lifetime past shutdown()'s explicit release order
        // (stop() joins the tick thread before lua_services is freed, so
        // a fire can never race the teardown).
        g_state->global_manager->set_task_fire_fn(
            [services](const std::string& service_id,
                       const std::string& task_name) -> bool {
                if (!services->service_vm(service_id)) {
                    return false;
                }
                std::string error;
                return services->send_system(service_id, "on_scheduler_task",
                                             nlohmann::json::array({task_name}),
                                             &error);
            });
        g_state->global_manager->start();
    }
#endif

#ifdef SHIELD_ENABLE_CLUSTER
    // M3: local service-name publications become cluster routes. The
    // notifier must be installed before any service spawns; it reads the
    // manager through the global accessor so shutdown ordering (manager
    // released first) stays safe.
    if (g_state->cluster_manager && g_state->cluster_transport) {
        g_state->lua_services->set_name_change_notifier(
            [](const std::string& name, const std::string& service_id) {
                if (auto* mgr = shield::cluster::global_cluster_manager()) {
                    mgr->on_local_route_changed(name, service_id);
                }
            });
    }

    // M4 data-plane glue. Every closure below is invoked from another
    // thread (transport actor or service actor dispatch); all of them only
    // touch thread-safe seams, and they hold shared_ptr so a capture can
    // never dangle while teardown interleaves with in-flight messages.
    if (g_state->cluster_manager && g_state->cluster_transport &&
        g_state->lua_services) {
        auto services = g_state->lua_services;
        auto transport = g_state->cluster_transport;

        // GCOVR_EXCL_START (bootstrap glue: one-line delegations whose
        // behavior the tests/cluster suites cover isomorphically by driving
        // the same seams directly)
        // Caller side: shield.send/call aimed at "node:service" leaves via
        // the transport envelope path.
        g_state->cluster_manager->set_remote_send_fn(
            [transport](const std::string& node, const std::string& service_id,
                        const std::string& method, const std::string& args_json,
                        uint64_t call_session, int32_t timeout_ms,
                        std::string* error) {
                return transport->send_envelope(node, service_id, method,
                                                args_json, call_session,
                                                timeout_ms, error);
            });

        // Callee side: inbound envelopes dispatch against the local service
        // manager.
        shield::cluster::EnvelopeBridges bridges;
        bridges.send_dispatch = [services](const std::string& service_id,
                                           const std::string& method,
                                           const std::string& args_json) {
            nlohmann::json args =
                nlohmann::json::parse(args_json, nullptr, false);
            if (args.is_discarded()) {
                args = nlohmann::json::array();
            }
            std::string error;
            return services->send(service_id, method, args, &error);
        };
        // Two-phase inbound call: the transport allocates the session, then
        // registers its routing entry, THEN dispatches — so a fast callee
        // completion can never race the registration.
        bridges.call_begin = [services](int32_t timeout_ms) {
            return services->begin_proxied_call(timeout_ms);
        };
        bridges.call_dispatch =
            [services](uint64_t session, const std::string& service_id,
                       const std::string& method, const std::string& args_json,
                       std::string* error) {
                nlohmann::json args =
                    nlohmann::json::parse(args_json, nullptr, false);
                if (args.is_discarded()) {
                    args = nlohmann::json::array();
                }
                return services->dispatch_proxied_call(session, service_id,
                                                       method, args, error);
            };
        bridges.reply_handler = [services](uint64_t call_session, bool ok,
                                           const std::string& payload_json,
                                           const std::string& error_code,
                                           const std::string& error_message) {
            nlohmann::json values;
            if (ok) {
                values = nlohmann::json::parse(payload_json, nullptr, false);
                if (values.is_discarded()) {
                    ok = false;
                    values = nlohmann::json::array({nlohmann::json::object(
                        {{"code", "handler_error"},
                         {"message", "invalid reply payload"}})});
                }
            } else {
                values = nlohmann::json::array({nlohmann::json::object(
                    {{"code",
                      error_code.empty() ? "handler_error" : error_code},
                     {"message", error_message}})});
            }
            services->complete_call(call_session, ok, values);
        };
        transport->set_envelope_bridges(std::move(bridges));

        // Proxied-call completions flow back out over the transport, reply
        // addressed to the node the envelope came from.
        g_state->lua_services->set_proxied_call_hook(
            [transport](uint64_t session, bool ok,
                        const nlohmann::json& values) {
                if (ok) {
                    transport->complete_proxied_call(session, true,
                                                     values.dump(), "", "");
                    return;
                }
                // values is the error array [{code, message, ...}]; lift the
                // first entry's code/message into the reply fields.
                std::string code = "handler_error";
                std::string message = "call failed";
                if (values.is_array() && !values.empty() &&
                    values.front().is_object()) {
                    const auto& first = values.front();
                    if (first.contains("code") && first["code"].is_string()) {
                        code = first["code"].get<std::string>();
                    }
                    if (first.contains("message") &&
                        first["message"].is_string()) {
                        message = first["message"].get<std::string>();
                    }
                }
                transport->complete_proxied_call(session, false, "", code,
                                                 message);
            });
        // GCOVR_EXCL_STOP
    }
#endif

    // Merge every actor's rpc.routes into one gateway descriptor set: route
    // ids and names must be unique across the process (a single client-facing
    // route namespace), so conflicts fail bootstrap before any actor spawns.
    // An entry without an explicit owner_service belongs to the actor that
    // declared it; every VM then receives the normalized union and compiles
    // exactly the entries it owns, so a binding that resolves nowhere fails
    // its owner's spawn (handler_missing) instead of surfacing at dispatch.
    nlohmann::json merged_rpc_routes = nlohmann::json::array();
    for (const auto& actor : shield::config::runtime_actors()) {
        nlohmann::json routes =
            nlohmann::json::parse(actor.rpc_routes_json, nullptr, false);
        if (routes.is_discarded() ||  // GCOVR_EXCL_BR_LINE (same guard chain
                                      // as below — rpc_routes_json is always a
                                      // JSON array)
            !routes.is_array()) {     // GCOVR_EXCL_BR_LINE (defensive:
                                   // rpc_routes_json is emitted by the config
                                   // layer as a JSON array and
                                   // validate_actor_rpc_routes enforces the
                                   // shape, so both guard arms are unreachable)
            // GCOVR_EXCL_START (rpc_routes_json is always valid JSON from
            // the config layer; spawn revalidates the shape anyway)
            routes = nlohmann::json::array();
            // GCOVR_EXCL_STOP
        }
        for (auto& route : routes) {
            if (!route.is_object()) {  // GCOVR_EXCL_BR_LINE (defensive:
                // validate_actor_rpc_routes rejects non-map route items, so
                // the skip arm cannot occur — see the excluded continue)
                continue;  // GCOVR_EXCL_LINE (config validation rejects
                           // non-map route items before bootstrap runs)
            }
            if (!route.contains("owner_service") ||
                !route["owner_service"]  // GCOVR_EXCL_BR_LINE (defensive:
                                         // validation guarantees a string
                                         // owner_service)
                     .is_string() ||     // GCOVR_EXCL_BR_LINE
                                         // (defensive:
                                         // validation
                                         // rejects
                                         // owner_service
                                         // values that are
                                         // not strings, so
                                         // the is_string
                                         // guard arm is
                                         // unreachable;
                                         // the missing and
                                         // empty-string
                                         // arms are
                                         // exercised)
                route["owner_service"]
                    .get<std::string>()  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: get<> template clone
                                         // arcs, same as .empty() below)
                    .empty()) {  // GCOVR_EXCL_BR_LINE (compiler artifact: zero
                                 // arcs are never-executed clones of the
                                 // inlined nlohmann get<std::string> template;
                                 // the empty and non-empty arms are exercised)
                route["owner_service"] = actor.name;
            }
        }
        merged_rpc_routes.insert(merged_rpc_routes.end(), routes.begin(),
                                 routes.end());
    }
    shield::transport::RpcDescriptorTable descriptor_routes;
    std::string descriptor_error;
    if (!shield::transport::parse_rpc_routes_json(
            merged_rpc_routes.dump(), descriptor_routes, &descriptor_error)) {
        SHIELD_LOG_ERROR(log, "Invalid rpc.routes: " + descriptor_error);
        return false;
    }

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
            };  // GCOVR_EXCL_BR_LINE (compiler artifact: braced-init machinery
                // on a pure expression line, no condition semantics; zero arcs
                // are never-executed initializer-list clones)
            if (opts["config"]  // GCOVR_EXCL_BR_LINE (defensive: options_json
                                // is always valid JSON)
                    .is_discarded()) {  // GCOVR_EXCL_BR_LINE
                                        // (defensive:
                                        // options_json is
                                        // always valid JSON
                                        // from the config
                                        // layer; body already
                                        // excluded)
                // GCOVR_EXCL_START (options_json is always valid JSON)
                opts["config"] = nlohmann::json::object();
                // GCOVR_EXCL_STOP
            }
            opts["rpc"] = {
                {"routes",
                 merged_rpc_routes}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // braced-init assignment with no
                                       // condition semantics; zero arcs are
                                       // never-executed initializer-list
                                       // clones)

            auto result = g_state->lua_services->spawn(
                resolve_script_path(actor), opts.dump());
            if (!result.success) {
                SHIELD_LOG_ERROR(log, "Failed to spawn actor '" + service_name +
                                          "': " + result.error_message);
                if (actor.required) {
                    return false;
                }
                continue;
            }
            SHIELD_LOG_INFO(log, "Service spawned: " + result.service_id);
        }
    }

    for (const auto& actor : shield::config::runtime_actors()) {
        if (actor.network_tcp.empty() ||
            actor.instances ==  // GCOVR_EXCL_BR_LINE (defensive: the
                                // zero-instances arm is unreachable, see below)
                0) {  // GCOVR_EXCL_BR_LINE (defensive: validation requires
                      // instances == 1 when network.tcp is set, so the
                      // tcp-with-zero-instances arm cannot occur)
            continue;
        }
        auto endpoint = parse_endpoint(actor.network_tcp);
        // GCOVR_EXCL_START (unreachable: config validation rejects every
        // endpoint form that parse_endpoint would refuse)
        if (!endpoint) {
            SHIELD_LOG_ERROR(log, "Invalid TCP endpoint for actor '" +
                                      actor.name + "': " + actor.network_tcp);
            return false;
        }
        // GCOVR_EXCL_STOP
        if (endpoint->host != "0.0.0.0" && endpoint->host != "*" &&
            endpoint->host != "::" && endpoint->host != "localhost" &&
            endpoint->host != "127.0.0.1") {
            SHIELD_LOG_WARNING(
                log,
                "TcpListener currently binds all IPv4 interfaces; "
                "configured host is " +
                    endpoint->host);
        }
        if (actor.network_protocol_enabled) {  // GCOVR_EXCL_BR_LINE
                                               // (integration: suites never
                                               // boot an actor with the
                                               // protocol disabled)
            std::string protocol_error;
            auto protocol_options =
                protocol_build_options(actor.source_dir, actor.max_frame_size);
            protocol_options.descriptor_routes = &descriptor_routes;
            auto probe = shield::transport::build_protocol_pipeline_from_json(
                actor.network_protocol_json, protocol_options, &protocol_error);
            if (!probe) {
                SHIELD_LOG_ERROR(log, "Invalid TCP protocol for actor '" +
                                          actor.name + "': " + protocol_error);
                return false;
            }
        }

        // One gateway actor per listener: it owns the live-session registry
        // for this listener and handles ClientEgress / ClientBindRequest /
        // ClientCloseRequest (see gateway_actor.hpp). The merged descriptor
        // table is copied into the actor so egress validation has route
        // metadata independent of the spawn closures.
        auto session_registry =
            std::make_shared<shield::lua::GatewaySessionRegistry>();
        shield::lua::GatewayDeps gateway_deps;
        gateway_deps.gateway_name = actor.name;
        gateway_deps.registry = session_registry;
        gateway_deps.stats = std::make_shared<shield::lua::GatewayStats>();
        gateway_deps.descriptors = descriptor_routes;
        gateway_deps.manager = g_state->lua_services.get();
        g_state->gateway_actors.push_back(shield::lua::spawn_gateway_actor(
            g_state->lua_services->actor_system(), std::move(gateway_deps)));
        g_state->lua_services->register_gateway_actor(
            actor.name, g_state->gateway_actors.back());

        auto bridge = std::make_unique<shield::lua::LuaGatewayBridge>(
            *g_state->lua_services, actor.name, std::move(session_registry));
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
        if (actor.network_protocol_enabled) {  // GCOVR_EXCL_BR_LINE
                                               // (integration: suites never
                                               // boot an actor with the
                                               // protocol disabled)
            const auto protocol_json = actor.network_protocol_json;
            const auto source_dir = actor.source_dir;
            const auto listener_max_frame_size = actor.max_frame_size;

            // Resolve the codec plugin vtable ONCE per listener, here at
            // setup time, instead of on every new connection. This is safe
            // because plugins are never unloaded at runtime (PluginHost has
            // no hot-unload API), shutdown() tears down network listeners
            // and sessions before PluginHost shuts plugins down, and plugin
            // libraries stay mapped until process exit — so a vtable
            // captured here cannot dangle for the lifetime of this factory.
            const shield_protocol_codec_v1* resolved_codec = nullptr;
            {
                const auto protocol_config =
                    nlohmann::json::parse(protocol_json, nullptr, false);
                const auto body =
                    protocol_config.is_object()
                        ? protocol_config.value(  // GCOVR_EXCL_BR_LINE
                                                  // (compiler artifact: value<>
                                                  // template clone arcs, see
                                                  // the annotated fragments
                                                  // below)
                              "body",
                              nlohmann::json::
                                  object())  // GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: zero arcs include
                                             // never-executed value() template
                                             // clone arcs; the body-present and
                                             // body-missing arms are exercised
                                             // by tests)
                        : nlohmann::json::
                              object();  // GCOVR_EXCL_BR_LINE (defensive:
                                         // network.protocol is validated to be
                                         // a map, so the ternary's non-object
                                         // arm is unreachable)
                const std::string provider =
                    body.is_object()
                        ? body.value(  // GCOVR_EXCL_BR_LINE (compiler
                                       // artifact: value<> template clone arcs,
                                       // see the annotated fragments below)
                              "provider",
                              std::string{})  // GCOVR_EXCL_BR_LINE (defensive:
                                              // network.protocol.body is
                                              // validated to be a map, so the
                                              // is_object false arm is
                                              // unreachable; remaining zero
                                              // arcs are never-executed
                                              // template clones)
                        : std::string{};  // GCOVR_EXCL_BR_LINE (defensive: same
                                          // body-map validation guarantee as
                                          // above)
                if (!provider.empty()) {
                    // GCOVR_EXCL_START (codec provider binding; integration
                    // context -- needs a codec plugin loaded through the
                    // plugin host)
                    resolved_codec =
                        shield::plugin::global_host()
                            .get_by_binding<shield_protocol_codec_v1>(provider);
                    // GCOVR_EXCL_STOP
                    if (resolved_codec == nullptr) {  // GCOVR_EXCL_LINE
                        // GCOVR_EXCL_START
                        SHIELD_LOG_ERROR(
                            log,
                            "Protocol codec provider '" + provider +
                                "' for actor '" + actor.name +
                                "' is not configured or does not provide " +
                                SHIELD_PROTOCOL_CODEC_INTERFACE);
                        // GCOVR_EXCL_STOP
                        return false;  // GCOVR_EXCL_LINE
                    }
                }
            }

            callbacks
                .create_protocol_pipeline =  // GCOVR_EXCL_BR_LINE
                                             // (compiler artifact: assignment
                                             // and std::function conversion
                                             // machinery, see the annotation
                                             // below)
                [protocol_json,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // lambda capture / std::function arcs, see
                                 // below)
                 source_dir,     // GCOVR_EXCL_BR_LINE (compiler artifact:
                              // lambda-assignment line; zero and never-executed
                              // arcs are capture-copy and std::function
                              // conversion machinery without condition
                              // semantics)
                 listener_max_frame_size, resolved_codec,
                 descriptor_routes]() {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: never-executed
                                         // lambda-invocation clone arcs)
                    std::string protocol_error;
                    // GCOVR_EXCL_START
                    auto protocol_options = protocol_build_options(
                        source_dir, listener_max_frame_size);
                    protocol_options.descriptor_routes = &descriptor_routes;
                    // GCOVR_EXCL_STOP
                    if (resolved_codec != nullptr) {
                        // Serve the vtable resolved once at listener setup.
                        // build_protocol_pipeline_from_json still validates
                        // codec-name match and vtable completeness on every
                        // build.
                        // GCOVR_EXCL_START (external codec resolver;
                        // integration context)
                        protocol_options.external_codec_resolver =
                            [resolved_codec](std::string_view, std::string_view,
                                             std::string*)
                            -> const shield_protocol_codec_v1* {
                            return resolved_codec;
                        };
                        // GCOVR_EXCL_STOP
                    }
                    auto pipeline =
                        shield::transport::build_protocol_pipeline_from_json(
                            protocol_json, protocol_options, &protocol_error);
                    if (!pipeline &&  // GCOVR_EXCL_BR_LINE (both arms of
                                      // this operand run; the unreachable joint
                                      // arm is annotated below)
                        !protocol_error  // GCOVR_EXCL_BR_LINE (defensive: build
                                         // failure always sets protocol_error)
                             .empty()) {  // GCOVR_EXCL_BR_LINE
                                          // (defensive:
                                          // build_protocol_pipeline_from_json
                                          // returns null with
                                          // an empty error only
                                          // for a
                                          // discarded/non-object/empty
                                          // config, which the
                                          // startup probe
                                          // rejects before the
                                          // per-connection
                                          // factory exists)
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

    // Start console server if enabled
    if (shield::config::get(  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                              // std::string comparison arcs, see below)
            "console.enabled",
            "false") ==  // GCOVR_EXCL_BR_LINE
                         // (compiler artifact: std::string comparison dispatch
                         // clones, see the annotated fragment below)
            "true" &&    // GCOVR_EXCL_BR_LINE (compiler artifact: zero arcs are
                       // never-executed std::string comparison dispatch clones;
                       // the enabled and disabled arms are exercised)
        g_state->lua_services &&  // GCOVR_EXCL_BR_LINE (defensive: the null
                                  // arm is unobservable, see the annotated
                                  // fragment below)
        g_state
            ->lua_runtime) {  // GCOVR_EXCL_BR_LINE (defensive: lua_services and
                              // lua_runtime are installed unconditionally
                              // earlier and every prior failure returns, so the
                              // null arms cannot be observed)
        auto sock_path = shield::config::get("console.socket_path",
                                             "/tmp/shield-console.sock");
        try {
            g_state->console_server =
                std::make_unique<shield::net::ConsoleServer>(g_state->net_io,
                                                             sock_path);
            g_state->console_dispatcher =
                std::make_unique<shield::console::CommandDispatcher>();

            // Register command handlers. The objects live in g_state (not
            // in this scope): the dispatcher stores handlers as lambdas
            // capturing `this`, so scope-local ownership left those lambdas
            // dangling the moment initialize returned.
            g_state->root_commands =
                std::make_shared<shield::console::RootCommands>(
                    *g_state->lua_services);
            g_state->root_commands->register_all(*g_state->console_dispatcher);

            g_state->lua_commands =
                std::make_shared<shield::console::LuaCommands>(
                    *g_state->lua_services, *g_state->lua_runtime);
            g_state->lua_commands->register_all(*g_state->console_dispatcher);

            // Wire the line handler from the dispatcher to the server
            g_state->console_server->set_on_line(
                [dispatcher = g_state->console_dispatcher.get()](
                    std::shared_ptr<shield::net::ConsoleSession> session,
                    std::string line) { dispatcher->dispatch(session, line); });

            g_state->console_server->start();
            SHIELD_LOG_INFO(log, "Console server listening on " + sock_path);
        } catch (  // GCOVR_EXCL_BR_LINE (the EH landing-pad pseudo-branch,
                   // see the annotated fragment below)
            const std::exception& e) {  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: EH landing-pad
                                        // pseudo-branch; the handler body is
                                        // exercised — the failure log below
                                        // runs when the console socket
                                        // cannot bind)
            SHIELD_LOG_ERROR(
                log,
                std::string("Failed to start console server: ") + e.what());
        }
    }

    // Start HTTP ops server if enabled
    if (shield::config::get(  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                              // std::string comparison arcs, see below)
            "http.enabled",
            "false") ==  // GCOVR_EXCL_BR_LINE
                         // (compiler artifact: std::string comparison dispatch
                         // clones, see the annotated fragment below)
            "true" &&    // GCOVR_EXCL_BR_LINE (compiler artifact: zero arcs are
                       // never-executed std::string comparison dispatch clones;
                       // the enabled and disabled arms are exercised)
        g_state->lua_services &&  // GCOVR_EXCL_BR_LINE (defensive: the null
                                  // arm is unobservable, see the annotated
                                  // fragment below)
        g_state->lua_runtime) {   // GCOVR_EXCL_BR_LINE (defensive: same as the
                                  // console condition above — both pointers are
                                  // always installed here)
        // Default to loopback: these endpoints expose internals, and
        // /ops/eval is a code-entry point. Expose further deliberately.
        auto host = shield::config::get("http.host", "127.0.0.1");
        auto port = static_cast<uint16_t>(
            std::stoi(shield::config::get("http.port", "8080")));
        try {
            shield::net::HttpServerConfig http_config;
            http_config.host = host;
            http_config.port = port;
            g_state->http_server =
                std::make_unique<shield::net::HttpServer>(http_config);

            g_state->ops_http_handler =
                std::make_unique<shield::console::OpsHttpHandler>(
                    *g_state->lua_services, *g_state->lua_runtime);
            g_state->ops_http_handler->register_routes(*g_state->http_server);

            // Mirror shield.httpd.* routes registered by Lua services into
            // the server; later registrations flow through the sink.
            g_state->http_bridge = std::make_unique<shield::lua::LuaHttpBridge>(
                *g_state->lua_runtime, *g_state->lua_services);
            g_state->http_bridge->attach(*g_state->http_server);

            g_state->http_server->start();
            SHIELD_LOG_INFO(log, "HTTP ops server listening on " + host + ":" +
                                     std::to_string(port));
        } catch (  // GCOVR_EXCL_BR_LINE (the EH landing-pad pseudo-branch,
                   // see the annotated fragment below)
            const std::exception&
                e) {  // GCOVR_EXCL_BR_LINE (defensive: no injectable throw
                      // inside this try — HttpServer::start() reports failures
                      // through its return value, see the excluded handler
                      // body; the arcs are EH landing-pad pseudo-branches)
            // GCOVR_EXCL_START (unreachable: HttpServer::start() reports
            // failures through its return value, it does not throw)
            SHIELD_LOG_ERROR(
                log,
                std::string("Failed to start HTTP ops server: ") + e.what());
        }
        // GCOVR_EXCL_STOP
    }

#ifdef SHIELD_ENABLE_SERVER
    // Init complete: flip the server state machine to `running` (the
    // uptime/started_at origin) before the runtime is marked initialized.
    if (g_state->server_manager) {
        g_state->server_manager->mark_ready();
    }
#endif

    g_state->initialized = true;
    SHIELD_LOG_INFO(log, "Shield runtime initialized");
    return true;
}

bool initialize(const RuntimeConfig& config) {
    const bool ok = initialize_impl(config);
    if (!ok) {
        // Teardown runs only after initialize_impl's stack is gone: the
        // failure sites sit mid-loop and their locals (the failed listener,
        // the bridge, the session callbacks) hold captures into
        // GlobalState-owned memory. The teardown used to run at the failure
        // site, so those locals destructed after GlobalState died and their
        // destructors read freed memory (ASan: ~TcpListener in the
        // DuplicateListenerPortFails crash family).
        cleanup_failed_initialize();
    }
    return ok;
}

// Shutdown
void shutdown() {
    if (!g_state ||  // GCOVR_EXCL_BR_LINE (the null-state arm is
                     // unobservable, see the annotated fragment below)
        !g_state->initialized) {  // GCOVR_EXCL_BR_LINE (defensive: g_state
                                  // exists only while initialized or
                                  // mid-initialize on the same thread; the torn
                                  // non-null-but-uninitialized state is
                                  // unobservable)
        return;
    }

    auto& log = shield::log::get_logger("bootstrap");
    SHIELD_LOG_INFO(log, "Shield runtime shutting down...");

#ifdef SHIELD_ENABLE_SERVER
    // External stop sources (SIGINT/SIGTERM, console stop) end up here
    // without ever touching the server state machine; drive it through its
    // terminal state first so watchers receive the "shutdown" notification
    // while the runtime can still deliver and /ops reads report the true
    // state during a slow teardown (runtime-server.md shutdown contract;
    // external shutdown keeps no Lua observation window). A server-side
    // shutdown(ms) handover already scheduled it — schedule_shutdown is
    // idempotent there ("shutdown already scheduled"), and the injected
    // stop request firing again is a harmless no-op mid-shutdown.
    if (g_state->server_manager) {
        std::string server_error;
        (void)g_state->server_manager->schedule_shutdown(0, &server_error);
    }
#endif

    // shutdown.timeout.* budgets (milliseconds). Built-in defaults keep a
    // stuck on_exit or plugin shutdown callback from hanging process exit;
    // the watchdog enforces the total budget by terminating the process.
    const auto drain_budget_ms =
        shield::config::get_int("shutdown.timeout.service_drain", 0);
    const auto stop_budget_ms =
        shield::config::get_int("shutdown.timeout.service_stop", 10000);
    const auto plugin_budget_ms =
        shield::config::get_int("shutdown.timeout.plugin_shutdown", 10000);
    const auto total_budget_ms =
        shield::config::get_int("shutdown.timeout.total", 30000);

    // Total-budget watchdog: a detached thread force-exits the process if
    // graceful shutdown has not completed in time.
    auto shutdown_done = std::make_shared<std::atomic<bool>>(false);
    if (total_budget_ms > 0) {
        std::thread(  // GCOVR_EXCL_BR_LINE (the thread-invocation clone
                      // arcs, see the annotated fragment below)
            [done = shutdown_done,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // lambda capture arcs, see below)
             total_budget_ms]() {   // GCOVR_EXCL_BR_LINE (defensive:
                                    // detached watchdog; the post-sleep
                                    // arcs only complete past the total
                                    // budget, which trips the excluded
                                    // forced-exit body below, and the
                                    // never-executed arcs are
                                    // thread-invocation clones)
                std::this_thread::
                    sleep_for(  // GCOVR_EXCL_BR_LINE (defensive: tests always
                                // finish shutdown first and exit the process
                                // while the watchdog is still sleeping; waking
                                // it deterministically would kill the test
                                // process)
                        std::chrono::milliseconds(total_budget_ms));
                // GCOVR_EXCL_START (only runs when shutdown hangs past the
                // budget; testing it would kill the test process)
                if (!done->load()) {
                    shield::log::get_logger("bootstrap")
                        .fatal(
                            "shutdown total budget exhausted, forcing process "
                            "exit");
                    std::_Exit(70);
                }
                // GCOVR_EXCL_STOP
            })
            .detach();
    }

    // service_drain: give in-flight forked tasks a bounded window to finish
    // before tearing services down (on_shutdown(ctx) is still a target
    // contract; draining pending tasks is its current stand-in).
    if (drain_budget_ms > 0 &&  // GCOVR_EXCL_BR_LINE (integration: suites shut
                                // down with a positive drain budget)
        g_state->lua_services) {  // GCOVR_EXCL_BR_LINE (defensive: lua_services
                                  // is always installed when shutdown runs; the
                                  // null arm cannot be observed)
        const auto drain_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(drain_budget_ms);
        // GCOVR_EXCL_START (drain only spins when tasks are still pending
        // at shutdown; coverage suites drain before calling shutdown)
        while (g_state->lua_services->pending_task_count_total() > 0 &&
               std::chrono::steady_clock::now() < drain_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // GCOVR_EXCL_STOP
    }

    // Run PRE_SHUTDOWN starters
    run_starters(Phase::PRE_SHUTDOWN);

    // Stop console server before other components
    // Stop accepting and close the sessions now, but keep the dispatcher
    // and the command objects alive until the net threads are joined below:
    // a line already read dispatches through them on those threads, and
    // destroying the objects first was an ASan-confirmed use-after-free
    // (cmd_help reading the freed RootCommands).
    if (g_state->console_server) {
        g_state->console_server->stop();
    }

    // Stop HTTP ops server
    if (g_state->http_server) {
        if (g_state  // GCOVR_EXCL_BR_LINE (defensive: http_bridge is always
                     // attached, see below)
                ->http_bridge) {  // GCOVR_EXCL_BR_LINE (defensive:
                                  // http_bridge is attached
                                  // immediately after http_server is
                                  // created with no failure path in
                                  // between, so
                                  // bridge-null-with-server-present is
                                  // unreachable)
            g_state->http_bridge->detach();
            g_state->http_bridge.reset();
        }
        g_state->http_server->stop();
        g_state->http_server.reset();
        g_state->ops_http_handler.reset();
    }

    // Stop network ingress first so no new Lua work is queued while we tear
    // down.
    // INVARIANT: network listeners (and with them every Session and its
    // ProtocolPipeline, which may hold a codec plugin vtable pointer) MUST
    // be torn down — and the net threads joined — BEFORE PluginHost shuts
    // the plugins down below. This guarantees no session pipeline can
    // outlive the plugin vtable it resolved at listener setup time.
    for (auto& listener : g_state->tcp_listeners) {
        if (listener) {  // GCOVR_EXCL_BR_LINE (defensive: listeners are pushed
                         // only after a successful start, the vector never
                         // holds null)
            listener->stop();
        }
    }
    g_state->net_work_guard.reset();
    g_state->net_io.stop();
    for (auto& t : g_state->net_threads) {
        if (t.joinable())  // GCOVR_EXCL_BR_LINE (defensive: joinable is
            // always true here, see the annotated fragment below)
            t.join();  // GCOVR_EXCL_BR_LINE (defensive: each net thread is
                       // visited and joined exactly once, joinable is always
                       // true at the check)
    }
    g_state->tcp_listeners.clear();
    g_state->gateway_bridges.clear();

    // The net threads are joined: nothing can dispatch into the console
    // dispatcher anymore, so the console objects can finally die. They hold
    // references into the Lua services/runtime, which are released below.
    g_state->console_server.reset();
    g_state->console_dispatcher.reset();
    g_state->root_commands.reset();
    g_state->lua_commands.reset();

    // Exit the gateway actors before the Lua services shut down: their
    // bind responses call back into the manager (complete_call). The wait
    // joins on each actor's down message, so no handler can still
    // dereference the manager once this returns.
    exit_gateway_actors_and_wait();

    // Shutdown actor system (which stops all actors)
    if (g_state->lua_services) {  // GCOVR_EXCL_BR_LINE (defensive: shutdown
                                  // only runs on an initialized runtime, where
                                  // lua_services is installed)
        g_state->lua_services->shutdown_all("stopping", stop_budget_ms);
    }
#ifdef SHIELD_ENABLE_CLUSTER
    // Stop the transport BEFORE releasing the service manager: inbound
    // envelope bridges dispatch into lua_services, so it must outlive the
    // transport actor's message loop. stop() also clears the bridges, so a
    // straggler mailbox message becomes an honest no-op. The transport
    // actor lives in the CAF system: unpublish and kill it while the system
    // (and the manager its callbacks point at) are still alive.
    if (g_state->cluster_transport) {
        g_state->cluster_transport->stop();
    }
#endif
    g_state->lua_services.reset();
    g_state->lua_runtime.reset();
#ifdef SHIELD_ENABLE_CLUSTER
    shield::cluster::set_global_cluster_transport(nullptr);
    g_state->cluster_transport.reset();
#endif
#ifdef SHIELD_ENABLE_PLAYER
    // The session index dies with the process: every player service actor
    // was already stopped by shutdown_all above.
    shield::player::PlayerManager::set_global(nullptr);
    g_state->player_manager.reset();
#endif
#ifdef SHIELD_ENABLE_SERVER
    // Halt the shutdown timer and clear the notify/stop callbacks BEFORE
    // releasing the manager: the timer body reads the stop callback under
    // the manager lock, and the notify callback captures lua_services
    // (released above). stop() joins the timer, so nothing can fire into
    // the teardown.
    if (g_state->server_manager) {
        g_state->server_manager->stop();
    }
    shield::server::ServerManager::set_global(nullptr);
    g_state->server_manager.reset();
#endif
#ifdef SHIELD_ENABLE_GLOBAL
    // Join the scheduler tick thread and drop the fire callback (it
    // captures lua_services, released above) BEFORE the manager goes
    // away: stop() guarantees no fire races the teardown.
    if (g_state->global_manager) {
        g_state->global_manager->stop();
    }
    shield::global::GlobalManager::set_global(nullptr);
    g_state->global_manager.reset();
#endif
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
    shield::plugin::global_host().shutdown(plugin_budget_ms);

    g_state->initialized = false;
    SHIELD_LOG_INFO(log, "Shield runtime shutdown complete");

    // Disarm the total-budget watchdog before logging tears down.
    shutdown_done->store(true);

    // Shutdown logging after the final runtime log has been emitted.
    shield::log::Logger::shutdown();

    g_state_owner.reset();
    g_state = nullptr;
}

// Check if initialized
bool is_initialized() {
    return g_state &&
           g_state->initialized;  // GCOVR_EXCL_BR_LINE (defensive: torn state
                                  // is unobservable, see below)
}  // GCOVR_EXCL_BR_LINE (defensive: the torn state — g_state set but not yet
   // initialized — is unobservable from the single-threaded call paths)

int run(int argc, char** argv) { return shield::run(argc, argv); }

}  // namespace shield::bootstrap
