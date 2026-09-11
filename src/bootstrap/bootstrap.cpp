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
#include <algorithm>
#include <atomic>
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
#include "shield/console/command_dispatcher.hpp"
#include "shield/console/lua_commands.hpp"
#include "shield/console/ops_http_handler.hpp"
#include "shield/console/root_commands.hpp"
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
    std::vector<std::unique_ptr<shield::net::TcpListener>> tcp_listeners;
    std::unique_ptr<shield::net::ConsoleServer> console_server;
    std::unique_ptr<shield::console::CommandDispatcher> console_dispatcher;
    std::unique_ptr<shield::net::HttpServer> http_server;
    std::unique_ptr<shield::console::OpsHttpHandler> ops_http_handler;
    std::unique_ptr<shield::lua::LuaHttpBridge> http_bridge;
#ifdef SHIELD_ENABLE_CLUSTER
    std::unique_ptr<shield::cluster::ClusterManager> cluster_manager;
    std::shared_ptr<shield::cluster::ClusterTransport> cluster_transport;
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
            // GCOVR_EXCL_START (unreachable: no initialize() failure happens
            // after the console server starts)
            g_state->console_server->stop();
            g_state->console_server.reset();
            g_state->console_dispatcher.reset();
            // GCOVR_EXCL_STOP
        }
        if (g_state->lua_services) {
            g_state->lua_services->shutdown_all("startup_failed");
        }
        g_state->lua_services.reset();
        g_state->lua_runtime.reset();
        shield::plugin::global_host().shutdown();
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
#ifdef SHIELD_ENABLE_CLUSTER
    // Cluster wire types must be in CAF's global meta object table before
    // any actor_system is constructed (CAF requirement, not just convention).
    shield::cluster::init_cluster_caf_types();
#endif
    // GCOVR_EXCL_START (uncalled static-init clone)
    caf::actor_system_config& caf_config = [&]() -> auto& {
        // GCOVR_EXCL_STOP
        static caf::actor_system_config cfg;
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
            cleanup_failed_initialize();
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
        if (routes.is_discarded() || !routes.is_array()) {
            // GCOVR_EXCL_START (rpc_routes_json is always valid JSON from
            // the config layer; spawn revalidates the shape anyway)
            routes = nlohmann::json::array();
            // GCOVR_EXCL_STOP
        }
        for (auto& route : routes) {
            if (!route.is_object()) {
                // GCOVR_EXCL_LINE (config validation rejects non-map route
                // items before bootstrap runs)
                continue;  // shape errors are reported by the parser below
            }
            if (!route.contains("owner_service") ||
                !route["owner_service"].is_string() ||
                route["owner_service"].get<std::string>().empty()) {
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
        cleanup_failed_initialize();
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
            };
            if (opts["config"].is_discarded()) {
                // GCOVR_EXCL_START (options_json is always valid JSON)
                opts["config"] = nlohmann::json::object();
                // GCOVR_EXCL_STOP
            }
            opts["rpc"] = {{"routes", merged_rpc_routes}};

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
        // GCOVR_EXCL_START (unreachable: config validation rejects every
        // endpoint form that parse_endpoint would refuse)
        if (!endpoint) {
            SHIELD_LOG_ERROR(log, "Invalid TCP endpoint for actor '" +
                                      actor.name + "': " + actor.network_tcp);
            cleanup_failed_initialize();
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
        if (actor.network_protocol_enabled) {
            std::string protocol_error;
            auto protocol_options =
                protocol_build_options(actor.source_dir, actor.max_frame_size);
            protocol_options.descriptor_routes = &descriptor_routes;
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
                const auto body = protocol_config.is_object()
                                      ? protocol_config.value(
                                            "body", nlohmann::json::object())
                                      : nlohmann::json::object();
                const std::string provider =
                    body.is_object() ? body.value("provider", std::string{})
                                     : std::string{};
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
                        cleanup_failed_initialize();  // GCOVR_EXCL_LINE
                        return false;                 // GCOVR_EXCL_LINE
                    }
                }
            }

            callbacks.create_protocol_pipeline = [protocol_json, source_dir,
                                                  listener_max_frame_size,
                                                  resolved_codec,
                                                  descriptor_routes]() {
                std::string protocol_error;
                // GCOVR_EXCL_START
                auto protocol_options =
                    protocol_build_options(source_dir, listener_max_frame_size);
                protocol_options.descriptor_routes = &descriptor_routes;
                // GCOVR_EXCL_STOP
                if (resolved_codec != nullptr) {
                    // Serve the vtable resolved once at listener setup.
                    // build_protocol_pipeline_from_json still validates
                    // codec-name match and vtable completeness on every
                    // build.
                    // GCOVR_EXCL_START (external codec resolver; integration
                    // context)
                    protocol_options.external_codec_resolver =
                        [resolved_codec](
                            std::string_view, std::string_view,
                            std::string*) -> const shield_protocol_codec_v1* {
                        return resolved_codec;
                    };
                    // GCOVR_EXCL_STOP
                }
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

    // Start console server if enabled
    if (shield::config::get("console.enabled", "false") == "true" &&
        g_state->lua_services && g_state->lua_runtime) {
        auto sock_path = shield::config::get("console.socket_path",
                                             "/tmp/shield-console.sock");
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
                [&dispatcher](
                    std::shared_ptr<shield::net::ConsoleSession> session,
                    std::string line) { dispatcher.dispatch(session, line); });

            g_state->console_server->start();
            SHIELD_LOG_INFO(log, "Console server listening on " + sock_path);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR(
                log,
                std::string("Failed to start console server: ") + e.what());
        }
    }

    // Start HTTP ops server if enabled
    if (shield::config::get("http.enabled", "false") == "true" &&
        g_state->lua_services && g_state->lua_runtime) {
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
        } catch (const std::exception& e) {
            // GCOVR_EXCL_START (unreachable: HttpServer::start() reports
            // failures through its return value, it does not throw)
            SHIELD_LOG_ERROR(
                log,
                std::string("Failed to start HTTP ops server: ") + e.what());
            // GCOVR_EXCL_STOP
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
        std::thread([done = shutdown_done, total_budget_ms]() {
            std::this_thread::sleep_for(
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
        }).detach();
    }

    // service_drain: give in-flight forked tasks a bounded window to finish
    // before tearing services down (on_shutdown(ctx) is still a target
    // contract; draining pending tasks is its current stand-in).
    if (drain_budget_ms > 0 && g_state->lua_services) {
        const auto drain_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(drain_budget_ms);
        while (g_state->lua_services->pending_task_count_total() > 0 &&
               std::chrono::steady_clock::now() < drain_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Run PRE_SHUTDOWN starters
    run_starters(Phase::PRE_SHUTDOWN);

    // Stop console server before other components
    if (g_state->console_server) {
        g_state->console_server->stop();
        g_state->console_server.reset();
        g_state->console_dispatcher.reset();
    }

    // Stop HTTP ops server
    if (g_state->http_server) {
        if (g_state->http_bridge) {
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

    // Shutdown actor system (which stops all actors)
    if (g_state->lua_services) {
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
bool is_initialized() { return g_state && g_state->initialized; }

int run(int argc, char** argv) { return shield::run(argc, argv); }

}  // namespace shield::bootstrap
