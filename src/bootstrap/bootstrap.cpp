// [SHIELD_BOOTSTRAP] Bootstrap implementation
#include "shield/bootstrap/bootstrap.hpp"

#include "shield/base/error.hpp"
#include "shield/bootstrap/starter.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/plugin_host.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
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

std::optional<shield::transport::Endian> parse_protocol_endian(
    const nlohmann::json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto text = value.get<std::string>();
    if (text == "big" || text == "be") {
        return shield::transport::Endian::Big;
    }
    if (text == "little" || text == "le") {
        return shield::transport::Endian::Little;
    }
    return std::nullopt;
}

std::optional<shield::transport::EnvelopeKind> parse_envelope_kind(
    const std::string& value) {
    if (value == "lenprefix" || value == "len-prefix" ||
        value == "len_prefix") {
        return shield::transport::EnvelopeKind::LenPrefix;
    }
    if (value == "idlen" || value == "id-len" || value == "id_len") {
        return shield::transport::EnvelopeKind::IdLen;
    }
    if (value == "typed_len" || value == "type_len" ||
        value == "typed-len" || value == "typelen") {
        return shield::transport::EnvelopeKind::TypeLen;
    }
    if (value == "delimiter" || value == "line") {
        return shield::transport::EnvelopeKind::Delimiter;
    }
    return std::nullopt;
}

std::optional<shield::transport::RouteSource> parse_route_source(
    const std::string& value) {
    if (value == "header" || value == "header.route_id" ||
        value == "header.msg_id") {
        return shield::transport::RouteSource::Header;
    }
    if (value == "body" || value == "body.route" ||
        value == "body.route_id") {
        return shield::transport::RouteSource::Body;
    }
    if (value == "none") {
        return shield::transport::RouteSource::None;
    }
    return std::nullopt;
}

std::optional<shield::transport::RouteAction> parse_route_action(
    const std::string& value) {
    if (value == "decode" || value == "decode_local") {
        return shield::transport::RouteAction::DecodeLocal;
    }
    if (value == "forward" || value == "forward_raw") {
        return shield::transport::RouteAction::ForwardRaw;
    }
    if (value == "drop") {
        return shield::transport::RouteAction::Drop;
    }
    return std::nullopt;
}

shield::transport::RouteSource default_route_source_for_envelope(
    shield::transport::EnvelopeKind kind) {
    switch (kind) {
        case shield::transport::EnvelopeKind::IdLen:
        case shield::transport::EnvelopeKind::TypeLen:
            return shield::transport::RouteSource::Header;
        case shield::transport::EnvelopeKind::LenPrefix:
        case shield::transport::EnvelopeKind::Delimiter:
            return shield::transport::RouteSource::Body;
    }
    return shield::transport::RouteSource::Body;
}

std::unique_ptr<shield::transport::ProtocolPipeline> make_protocol_pipeline(
    const std::string& protocol_json, const std::string& source_dir,
    std::string* error) {
    try {
        nlohmann::json config =
            nlohmann::json::parse(protocol_json, nullptr, false);
        if (config.is_discarded() || !config.is_object() || config.empty()) {
            return nullptr;
        }

        shield::transport::ProtocolProfile profile;
        profile.name = config.value("name", std::string{});

        const auto envelope = config.value("envelope", nlohmann::json::object());
        if (envelope.is_object()) {
            if (envelope.contains("type")) {
                const auto parsed =
                    parse_envelope_kind(envelope["type"].get<std::string>());
                if (!parsed) {
                    if (error) *error = "unknown network.protocol.envelope.type";
                    return nullptr;
                }
                profile.envelope_kind = *parsed;
            }
            if (envelope.contains("endian")) {
                const auto parsed = parse_protocol_endian(envelope["endian"]);
                if (!parsed) {
                    if (error) *error = "invalid network.protocol.envelope.endian";
                    return nullptr;
                }
                profile.envelope.endian = *parsed;
            }
            profile.envelope.length_bytes =
                static_cast<std::uint8_t>(envelope.value("length_bytes", 0));
            profile.envelope.route_id_bytes =
                static_cast<std::uint8_t>(envelope.value("route_id_bytes", 0));
            profile.envelope.length_includes_header =
                envelope.value("length_includes_header", false);
            if (envelope.contains("delimiter") &&
                envelope["delimiter"].is_string()) {
                const auto delimiter = envelope["delimiter"].get<std::string>();
                if (!delimiter.empty()) {
                    profile.envelope.delimiter =
                        static_cast<std::uint8_t>(delimiter.front());
                }
            }
            profile.envelope.max_frame_size =
                envelope.value("max_frame_size", std::size_t{0});
        }
        profile.route_source = default_route_source_for_envelope(
            profile.envelope_kind);

        const auto body = config.value("body", nlohmann::json::object());
        const std::string body_codec = body.value("codec", "raw");

        shield::transport::BodyCodecRegistry codecs;
        constexpr std::uint16_t default_codec_id = 1;
        auto codec = shield::transport::create_body_codec(body_codec);
        if (!codec) {
            if (error) *error = "unsupported network.protocol.body.codec";
            return nullptr;
        }
        const std::string normalized_body_codec(codec->name());
        codecs.add(default_codec_id, std::move(codec));
        profile.default_codec_id = default_codec_id;

        const auto routing = config.value("routing", nlohmann::json::object());
        if (routing.is_object()) {
            if (routing.contains("source") && routing["source"].is_string()) {
                const auto parsed =
                    parse_route_source(routing["source"].get<std::string>());
                if (!parsed) {
                    if (error) *error = "invalid network.protocol.routing.source";
                    return nullptr;
                }
                profile.route_source = *parsed;
            }
            profile.decode_body_route = routing.value("decode_body_route", true);
            profile.decode_before_dispatch =
                routing.value("decode_before_dispatch", false);
            if (routing.contains("unknown_route_action") &&
                routing["unknown_route_action"].is_string()) {
                const auto parsed = parse_route_action(
                    routing["unknown_route_action"].get<std::string>());
                if (!parsed) {
                    if (error) {
                        *error =
                            "invalid network.protocol.routing.unknown_route_action";
                    }
                    return nullptr;
                }
                profile.unknown_route_action = *parsed;
            }
        }

        shield::transport::RouteTable routes;
        if (normalized_body_codec == "xmldef" && body.contains("catalog") &&
            body["catalog"].is_string()) {
            shield::transport::XmldefCatalogOptions catalog_options;
            catalog_options.default_codec_id = default_codec_id;
            catalog_options.default_action =
                routing.value("default_action", std::string{}) == "forward_raw"
                    ? shield::transport::RouteAction::ForwardRaw
                    : shield::transport::RouteAction::DecodeLocal;
            catalog_options.default_lazy_decode =
                routing.value("lazy_decode", true);

            std::filesystem::path catalog_path(
                body["catalog"].get<std::string>());
            if (catalog_path.is_relative() && !source_dir.empty()) {
                catalog_path = std::filesystem::path(source_dir) / catalog_path;
            }

            std::string catalog_error;
            if (!shield::transport::load_xmldef_routes_from_file(
                    catalog_path.string(), routes, catalog_options,
                    &catalog_error)) {
                if (error) {
                    *error = "failed to load xmldef catalog: " + catalog_error;
                }
                return nullptr;
            }
        }

        if (config.contains("routes") && config["routes"].is_array()) {
            for (const auto& route : config["routes"]) {
                if (!route.is_object()) {
                    continue;
                }
                shield::transport::RouteEntry entry;
                entry.route_id = route.value("id", std::uint32_t{0});
                entry.target_service =
                    route.value("target_service", std::uint32_t{0});
                entry.codec_id = route.value("codec_id", default_codec_id);
                entry.schema_id = route.value("schema_id", std::uint16_t{0});
                entry.debug_name = route.value("name", std::string{});
                entry.policy.lazy_decode = route.value("lazy_decode", true);
                if (route.contains("action") && route["action"].is_string()) {
                    const auto parsed =
                        parse_route_action(route["action"].get<std::string>());
                    if (!parsed) {
                        if (error) {
                            *error = "invalid network.protocol.routes.action";
                        }
                        return nullptr;
                    }
                    entry.policy.action = *parsed;
                }
                if (entry.route_id == 0) {
                    if (error) *error = "network.protocol.routes[].id is required";
                    return nullptr;
                }

                if (!entry.debug_name.empty()) {
                    if (const auto* named = routes.find_by_name(entry.debug_name);
                        named != nullptr && named->route_id != entry.route_id) {
                        if (error) {
                            *error =
                                "network.protocol.routes contains duplicate name";
                        }
                        return nullptr;
                    }
                }

                routes.upsert(std::move(entry));
            }
        }

        return std::make_unique<shield::transport::ProtocolPipeline>(
            std::move(profile), std::move(routes), std::move(codecs));
    } catch (const nlohmann::json::exception& ex) {
        if (error) {
            *error = std::string("invalid network.protocol: ") + ex.what();
        }
        return nullptr;
    }
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
    std::thread net_thread;
    std::vector<std::unique_ptr<shield::lua::LuaGatewayBridge>> gateway_bridges;
    std::vector<std::unique_ptr<shield::net::TcpListener>> tcp_listeners;
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
        g_state->net_io.stop();
        if (g_state->net_thread.joinable()) {
            g_state->net_thread.join();
        }
        g_state->tcp_listeners.clear();
        g_state->gateway_bridges.clear();
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
        if (actor.network_protocol_json != "{}") {
            std::string protocol_error;
            auto probe = make_protocol_pipeline(actor.network_protocol_json,
                                                actor.source_dir,
                                                &protocol_error);
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
        callbacks.on_message =
            [bridge_ptr = bridge.get()](
                std::shared_ptr<shield::net::Session> session,
                const std::vector<uint8_t>& payload) {
                bridge_ptr->on_message(
                    std::move(session),
                    std::string(payload.begin(), payload.end()));
            };
        callbacks.on_packet =
            [bridge_ptr = bridge.get()](
                std::shared_ptr<shield::net::Session> session,
                const shield::transport::DispatchResult& packet) {
                bridge_ptr->on_packet(std::move(session), packet);
            };
        callbacks.on_disconnect =
            [bridge_ptr = bridge.get()](
                std::shared_ptr<shield::net::Session> session,
                std::string_view reason) {
                bridge_ptr->on_disconnect(std::move(session),
                                          std::string(reason));
            };
        if (actor.network_protocol_json != "{}") {
            const auto protocol_json = actor.network_protocol_json;
            const auto source_dir = actor.source_dir;
            callbacks.create_protocol_pipeline =
                [protocol_json, source_dir]() mutable {
                    std::string protocol_error;
                    auto pipeline = make_protocol_pipeline(
                        protocol_json, source_dir, &protocol_error);
                    if (!pipeline && !protocol_error.empty()) {
                        auto& log = shield::log::get_logger("bootstrap");
                        SHIELD_LOG_ERROR(log, "Invalid network protocol: " +
                                                  protocol_error);
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
        listener->start();
        SHIELD_LOG_INFO(log, "TCP gateway listener started for actor '" +
                                 actor.name + "' on " + actor.network_tcp);
        g_state->gateway_bridges.push_back(std::move(bridge));
        g_state->tcp_listeners.push_back(std::move(listener));
    }

    if (!g_state->tcp_listeners.empty()) {
        g_state->net_thread = std::thread([]() { g_state->net_io.run(); });
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
    for (auto& listener : g_state->tcp_listeners) {
        if (listener) {
            listener->stop();
        }
    }
    g_state->net_io.stop();
    if (g_state->net_thread.joinable()) {
        g_state->net_thread.join();
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
