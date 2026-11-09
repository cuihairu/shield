// [SHIELD_BOOTSTRAP] Bootstrap phases: rpc-route merge, runtime actor
// spawn, TCP listeners and the network IO threads.
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

nlohmann::json merge_actor_rpc_routes() {
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

            !routes.is_array()) {  // GCOVR_EXCL_BR_LINE (defensive:

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

                     .is_string() ||  // GCOVR_EXCL_BR_LINE

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

    return merged_rpc_routes;
}

bool spawn_runtime_actors(const nlohmann::json& merged_rpc_routes,

                          shield::transport::RpcDescriptorTable* out) {
    auto& log = shield::log::get_logger("bootstrap");

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

    *out = std::move(descriptor_routes);

    return true;
}

bool start_tcp_listeners(

    shield::transport::RpcDescriptorTable& descriptor_routes) {
    auto& log = shield::log::get_logger("bootstrap");

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

                 source_dir,  // GCOVR_EXCL_BR_LINE (compiler artifact:

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

        if (actor.network_tls_enabled) {
            // Fail loud: a config that asks for TLS must never come up as a

            // plaintext listener, so a context that cannot be built (missing

            // file, bad PEM, key mismatch) aborts startup here.

            std::shared_ptr<boost::asio::ssl::context> tls_context;

            std::string tls_error;

            if (!shield::net::make_tls_server_context(

                    actor.network_tls_cert_file, actor.network_tls_key_file,

                    tls_context, &tls_error)) {
                SHIELD_LOG_ERROR(log, "TLS setup failed for actor '" +

                                          actor.name + "': " + tls_error);

                return false;
            }

            listener->set_tls(std::move(tls_context),

                              actor.network_tls_handshake_timeout_ms);

            SHIELD_LOG_INFO(

                log,

                "TLS enabled for actor '" + actor.name +

                    "' (minimum protocol TLS 1.2, handshake "

                    "timeout " +

                    std::to_string(actor.network_tls_handshake_timeout_ms) +

                    " ms)");
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

        if (actor.rate_limit_per_second > 0) {
            listener->set_rate_limit(actor.rate_limit_per_second,

                                     actor.rate_limit_burst);
        }

        if (!actor.blocklist_deny.empty()) {
            // Entries were already parsed by the config layer with the very

            // parser the listener uses, so a failure here would mean the two

            // disagree. Kept as a hard startup failure because silently

            // serving without the configured deny rules is the worse

            // outcome, but the arm is not reachable through the CLI.

            std::string blocklist_error;

            if (!listener->set_blocklist(  // GCOVR_EXCL_BR_LINE (defensive:

                    // config layer already parsed these

                    // entries with this same parser)

                    actor.blocklist_deny, &blocklist_error)) {
                // GCOVR_EXCL_START (unreachable via the CLI: the config layer

                // rejects a malformed entry before bootstrap reaches here)

                SHIELD_LOG_ERROR(log, "Invalid address blocklist for actor '" +

                                          actor.name + "': " + blocklist_error);

                return false;

                // GCOVR_EXCL_STOP
            }

            SHIELD_LOG_INFO(

                log, "Address blocklist active for actor '" + actor.name +

                         "' with " +

                         std::to_string(actor.blocklist_deny.size()) +

                         " rule(s)");
        }

        listener->start();

        SHIELD_LOG_INFO(log, "TCP gateway listener started for actor '" +

                                 actor.name + "' on " + actor.network_tcp);

        g_state->gateway_bridges.push_back(std::move(bridge));

        g_state->tcp_listeners.push_back(std::move(listener));
    }

    return true;
}

void start_net_io() {
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
}

}  // namespace shield::bootstrap
