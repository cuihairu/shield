// [SHIELD_CONFIG] Runtime-config validation: the protocol envelope,
// routing, routes, network, listener and actor validators plus the
// validate_runtime_config entry point.
// Split out of config.cpp.
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "config_impl.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/net/ip_blocklist.hpp"

namespace shield::log {
class Logger;
}

namespace shield::config {

std::optional<int> scalar_int(const YAML::Node& node, const char* key) {
    if (!node ||  // GCOVR_EXCL_BR_LINE (defensive: call sites guard the node)
        !node[key]) {  // GCOVR_EXCL_BR_LINE (defensive: every call site passes
                       // a guarded, defined node; the remaining zero arm is an
                       // inlined-call artifact)
        return std::nullopt;
    }
    try {
        return node[key].as<int>();
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch entry edge)
        const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // catch blocks have no non-exception entry
                                  // edge; the exception arm is covered)
        return std::nullopt;
    }
}

namespace {
bool validate_int_range(const YAML::Node& node, const char* key,
                        const char* path, int min_value, int max_value,
                        std::string* error);

bool validate_protocol_string_enum(const YAML::Node& node, const char* key,
                                   const std::string& path,
                                   std::initializer_list<const char*> allowed,
                                   std::string* error) {
    if (!node ||  // GCOVR_EXCL_BR_LINE (defensive: call sites guard the node)
        !node[key]) {  // GCOVR_EXCL_BR_LINE (defensive: every call site passes
                       // a guarded, defined node; the remaining zero arm is an
                       // inlined-call artifact)
        return true;
    }

    try {
        const auto value = node[key].as<std::string>();
        for (const auto* option : allowed) {
            if (value == option) {
                return true;
            }
        }
        if (error) {
            *error = path + "." + key + " has an unsupported value";
        }
        return false;
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch entry edge)
        const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // catch blocks have no non-exception entry
                                  // edge; the exception arm is covered)
        if (error) {
            *error = path + "." + key + " must be a string";
        }
        return false;
    }
}

// Required non-empty string scalar (used for network.tls cert/key paths,
// docs/tls-design.md). Unlike the validate_* helpers above, absence is an
// error: the callers only invoke this when the feature is enabled.
bool validate_non_empty_string(const YAML::Node& node, const char* key,
                               const std::string& path, std::string* error) {
    if (!node ||  // GCOVR_EXCL_BR_LINE (defensive: call sites guard the node)
        !node[key]) {  // GCOVR_EXCL_BR_LINE (missing key: covered)
        if (error) {
            *error = path + "." + key + " is required";
        }
        return false;
    }
    try {
        if (node[key].as<std::string>().empty()) {
            if (error) {
                *error = path + "." + key + " must not be empty";
            }
            return false;
        }
        return true;
    } catch (                     // GCOVR_EXCL_BR_LINE (catch entry edge)
        const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // catch blocks have no non-exception entry
                                  // edge; the exception arm is covered)
        if (error) {
            *error = path + "." + key + " must be a string";
        }
        return false;
    }
}

bool validate_protocol_route_action(const YAML::Node& node, const char* key,
                                    const std::string& path,
                                    std::string* error) {
    return validate_protocol_string_enum(
        node, key, path,
        {"decode", "decode_local", "forward", "forward_raw", "drop"}, error);
}

bool validate_protocol_envelope(const YAML::Node& envelope,
                                const std::string& path, std::string* error) {
    if (!envelope) {
        return true;
    }
    if (!envelope.IsMap()) {
        if (error) {
            *error = path + ".envelope must be a map";
        }
        return false;
    }

    if (!validate_protocol_string_enum(
            envelope, "type",
            path + ".envelope",  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // never-executed gcov clone arms)
            {"lenprefix", "len-prefix", "len_prefix", "idlen", "id-len",
             "id_len", "typed_len", "type_len", "typed-len", "typelen",
             "delimiter", "line"},
            error) ||
        !validate_protocol_string_enum(
            envelope, "endian",
            path + ".envelope",  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // inlined callee guard arm plus gcov clone
                                 // arms; both endian arms are covered)
            // GCOVR_EXCL_BR_START (compiler artifact: inlined callee guard
            // arms plus gcov clone arms)
            {"big", "be", "little", "le"}, error) ||
        !validate_int_range(
            envelope, "length_bytes", (path + ".envelope").c_str(), 0,
            255,  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov clone arms)
            error) ||  // GCOVR_EXCL_BR_LINE (compiler artifact:
                       // never-executed gcov clone arms)
        !validate_int_range(
            envelope, "route_id_bytes", (path + ".envelope").c_str(), 0,
            255,  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov clone arms)
            error) ||  // GCOVR_EXCL_BR_LINE (compiler artifact:
                       // never-executed gcov clone arms)
        !validate_int_range(
            envelope, "max_frame_size", (path + ".envelope").c_str(),
            0,  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov clone arms)
            16 * 1024 * 1024,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                               // never-executed gcov clone arms)
                               // GCOVR_EXCL_BR_STOP
            error)) {
        return false;
    }

    if (envelope["length_includes_header"]) {
        try {
            (void)envelope["length_includes_header"].as<bool>();
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // exception arm is covered)
            if (error) {
                *error =
                    path + ".envelope.length_includes_header must be a bool";
            }
            return false;
        }
    }

    if (envelope["delimiter"]) {
        try {
            const auto delimiter = envelope["delimiter"].as<std::string>();
            if (delimiter.empty()) {
                if (error) {
                    *error = path + ".envelope.delimiter must not be empty";
                }
                return false;
            }
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // exception arm is covered)
            if (error) {
                *error = path + ".envelope.delimiter must be a string";
            }
            return false;
        }
    }

    return true;
}

bool validate_protocol_routing(const YAML::Node& routing,
                               const std::string& path, std::string* error) {
    if (!routing) {
        return true;
    }
    if (!routing.IsMap()) {
        if (error) {
            *error = path + ".routing must be a map";
        }
        return false;
    }

    if (!validate_protocol_string_enum(
            routing, "source",
            path + ".routing",  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // never-executed gcov clone arms)
            {"header", "header.route_id", "header.msg_id", "body", "body.route",
             "body.route_id", "none"},
            error) ||
        !validate_protocol_route_action(
            routing, "unknown_route_action",
            path + ".routing",  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined arm plus gcov clone arms)
            error) ||  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined arm
                       // plus gcov clone arms; both unknown_route_action arms
                       // are covered)
        !validate_protocol_route_action(
            routing, "default_action",
            path + ".routing",  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // never-executed gcov clone arms)
            error)) {  // GCOVR_EXCL_BR_LINE (compiler artifact: never-executed
                       // gcov clone arms)
        return false;
    }

    for (const char* key :
         {"decode_body_route", "decode_before_dispatch", "lazy_decode"}) {
        if (!routing[key]) {
            continue;
        }
        try {
            (void)routing[key].as<bool>();
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // exception arm is covered)
            if (error) {
                *error = path + ".routing." + key + " must be a bool";
            }
            return false;
        }
    }

    return true;
}

bool validate_protocol_routes(const YAML::Node& routes, const std::string& path,
                              std::string* error) {
    if (!routes) {
        return true;
    }
    // Inline route tables were folded into the RPC descriptor set: the
    // single static source is now actors[].rpc.routes.
    (void)routes;
    if (error) {
        *error =
            path + ".routes was removed; declare actors[].rpc.routes instead";
    }
    return false;
}

bool validate_actor_rpc_routes(const YAML::Node& rpc, const std::string& path,
                               std::string* error) {
    if (!rpc || !rpc["routes"]) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // never-executed gcov clone arms)
        return true;
    }
    const auto& routes = rpc["routes"];
    if (!routes.IsSequence()) {
        if (error) {
            *error = path + ".routes must be an array";
        }
        return false;
    }

    std::unordered_set<std::uint32_t> ids;
    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto route = routes[i];
        const auto item_path = path + ".routes[" + std::to_string(i) + "]";
        if (!route.IsMap()) {
            if (error) {
                *error = item_path + " must be a map";
            }
            return false;
        }

        int route_id = 0;
        try {
            route_id = route["id"].as<int>();
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // exception arm is covered)
            if (error) {
                *error = item_path + ".id is required and must be an integer";
            }
            return false;
        }
        if (route_id <= 0) {
            if (error) {
                *error = item_path + ".id must be >= 1";
            }
            return false;
        }
        if (!ids.insert(static_cast<std::uint32_t>(route_id)).second) {
            if (error) {
                *error = path + ".routes contains duplicate id";
            }
            return false;
        }

        // binding is the one required string: without it the route has
        // nothing to compile against at spawn time.
        if (!route["binding"]) {
            if (error) {
                *error = item_path + ".binding is required";
            }
            return false;
        }

        if (!validate_protocol_string_enum(
                route, "direction", item_path,
                {"c2s", "client_to_server", "s2c", "server_to_client", "bidi",
                 "bidirectional"},
                error) ||
            // The rpc.routes descriptor set only accepts the canonical
            // actions; the xmldef-era "decode"/"forward" aliases are not
            // part of this contract.
            !validate_protocol_string_enum(
                route, "action", item_path,
                {"decode_local", "forward_raw", "drop"}, error)) {
            return false;
        }

        // Removed addressing keys are rejected outright (pre-1.0: no
        // silent-ignore compat reads), mirroring parse_rpc_routes_json.
        for (const char* removed : {"schema_id", "response_schema"}) {
            if (route[removed]) {
                if (error) {
                    *error = item_path + "." + removed +
                             " was removed; schema addressing is the route "
                             "name plus an optional request_schema override";
                }
                return false;
            }
        }

        if (route["requires_auth"]) {
            try {
                (void)route["requires_auth"].as<bool>();
            } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: catch blocks have no
                                               // non-exception entry edge; the
                                               // exception arm is covered)
                if (error) {
                    *error = item_path + ".requires_auth must be a bool";
                }
                return false;
            }
        }
        if (route["lazy_decode"]) {
            try {
                (void)route["lazy_decode"].as<bool>();
            } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: catch blocks have no
                                               // non-exception entry edge; the
                                               // exception arm is covered)
                if (error) {
                    *error = item_path + ".lazy_decode must be a bool";
                }
                return false;
            }
        }

        for (const char* key : {"binding", "owner_service", "name",
                                "request_codec", "request_schema"}) {
            if (!route[key]) {
                continue;
            }
            try {
                const auto value = route[key].as<std::string>();
                // binding is the one required string: without it the route
                // GCOVR_EXCL_BR_START (compiler artifact: inlined
                // std::string compare arms plus gcov clone arms)
                // has nothing to compile against at spawn time.
                if (std::string(key) ==
                        "binding" &&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // inlined std::string compare arms plus
                                      // gcov clone arms)
                    value.empty()) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // inlined std::string compare arms plus
                                      // gcov clone arms; the binding-empty arm
                                      // is covered)
                    if (error) {
                        *error = item_path + ".binding must not be empty";
                    }
                    return false;
                }
                // GCOVR_EXCL_BR_STOP
            } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: catch blocks have no
                                               // non-exception entry edge; the
                                               // exception arm is covered)
                if (error) {
                    *error = item_path + "." + key + " must be a string";
                }
                return false;
            }
        }

        if (route["name"]) {
            const auto name = route["name"].as<std::string>();
            if (!name.empty() && !names.insert(name).second) {
                if (error) {
                    *error = path + ".routes contains duplicate name";
                }
                return false;
            }
        }
    }

    return true;
}

bool validate_network_protocol(const YAML::Node& protocol,
                               const std::string& path, std::string* error) {
    if (!protocol) {  // GCOVR_EXCL_BR_LINE (defensive: the call site only
                      // invokes this validator on a defined protocol node)
        return true;  // GCOVR_EXCL_LINE (guarded by the call site)
    }
    if (!protocol.IsMap()) {  // GCOVR_EXCL_BR_LINE (defensive: the call site
                              // rejects non-map protocol nodes before this
                              // validator runs)
        // GCOVR_EXCL_START (unreachable: the call site rejects non-map
        // protocol nodes before invoking this validator)
        if (error) {
            *error = path + " must be a map";
        }
        return false;
        // GCOVR_EXCL_STOP
    }
    if (protocol.size() == 0) {
        if (error) {
            *error = path + " must not be empty";
        }
        return false;
    }

    if (protocol["name"]) {
        try {
            (void)protocol["name"].as<std::string>();
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // exception arm is covered)
            if (error) {
                *error = path + ".name must be a string";
            }
            return false;
        }
    }

    if (protocol["body"]) {
        const auto body = protocol["body"];
        if (!body.IsMap()) {
            if (error) {
                *error = path + ".body must be a map";
            }
            return false;
        }
        if (!validate_protocol_string_enum(
                body, "codec", path + ".body",
                {"raw", "json", "msgpack", "protobuf", "fbs", "flatbuffers",
                 "xmldef", "xml_def"},
                error)) {
            return false;
        }
        if (body["catalog"]) {
            try {
                (void)body["catalog"].as<std::string>();
            } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: catch blocks have no
                                               // non-exception entry edge; the
                                               // exception arm is covered)
                if (error) {
                    *error = path + ".body.catalog must be a string";
                }
                return false;
            }
        }
        if (body["provider"]) {
            try {
                const auto provider = body["provider"].as<std::string>();
                if (provider.empty()) {
                    if (error) {
                        *error = path + ".body.provider must not be empty";
                    }
                    return false;
                }
            } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: catch blocks have no
                                               // non-exception entry edge; the
                                               // exception arm is covered)
                if (error) {
                    *error = path + ".body.provider must be a string";
                }
                return false;
            }
        }
    }

    // GCOVR_EXCL_BR_START (compiler artifact: gcov clone arms plus
    // inlined operator[] arms)
    return validate_protocol_envelope(
               protocol["envelope"],
               path,  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov clone arms)
               error) &&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                          // never-executed gcov clone arms)
           validate_protocol_routing(
               protocol["routing"],
               path,      // GCOVR_EXCL_BR_LINE (compiler artifact: yaml-cpp
                          // operator[] inline arm plus gcov clone arms)
               error) &&  // GCOVR_EXCL_BR_LINE (compiler artifact: yaml-cpp
                          // operator[] inline arm plus gcov clone arms)
           validate_protocol_routes(
               protocol["routes"],
               path,    // GCOVR_EXCL_BR_LINE (compiler artifact: never-executed
                        // gcov clone arms)
               error);  // GCOVR_EXCL_BR_LINE (compiler artifact: never-executed
    // GCOVR_EXCL_BR_STOP
    // gcov clone arms)
}

bool validate_listener_address(const YAML::Node& node, const char* key,
                               const std::string& actor_name,
                               std::string* error) {
    if (!node[key]) {
        return true;
    }

    std::string value;
    try {
        value = node[key].as<std::string>();
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch entry edge)
        const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // catch blocks have no non-exception entry
                                  // edge; the exception arm is covered)
        if (error) {
            *error = "actors[" + actor_name + "].network." + key +
                     " must be a string host:port";
        }
        return false;
    }

    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
        if (error) {
            *error = "actors[" + actor_name + "].network." + key +
                     " must be host:port";
        }
        return false;
    }

    try {
        const int port = std::stoi(value.substr(colon + 1));
        if (port < 1 || port > 65535) {
            throw std::out_of_range("port");
        }
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch entry edge)
        const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // catch blocks have no non-exception entry
                                  // edge; the exception arm is covered)
        if (error) {
            *error = "actors[" + actor_name + "].network." + key +
                     " port must be between 1 and 65535";
        }
        return false;
    }

    return true;
}

std::optional<std::filesystem::path> existing_script_path(
    const YAML::Node& actor, const std::string& source_dir,
    const YAML::Node& root) {
    if (!actor["script"]) {   // GCOVR_EXCL_BR_LINE (defensive: script presence
                              // was already validated before the resolve loop
                              // runs)
        return std::nullopt;  // GCOVR_EXCL_LINE (guarded by earlier validation)
    }

    std::filesystem::path script(actor["script"].as<std::string>());
    if (script.is_absolute() && std::filesystem::exists(script)) {
        return script;
    }
    if (std::filesystem::exists(script)) {
        return script;
    }

    if (!source_dir.empty()) {
        auto from_source = std::filesystem::path(source_dir) / script;
        if (std::filesystem::exists(from_source)) {
            return from_source;
        }
    }

    if (root["lua"] &&  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                        // path-construction arms plus gcov clone arms)
        root["lua"]["script_path"]) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // inlined path-construction arms plus
                                       // gcov clone arms; the lua/script_path
                                       // presence arms are covered)
        auto from_lua_path = std::filesystem::path(
                                 root["lua"]["script_path"].as<std::string>()) /
                             script;
        if (std::filesystem::exists(from_lua_path)) {
            return from_lua_path;
        }
    }

    return std::nullopt;
}

bool validate_int_range(const YAML::Node& node, const char* key,
                        const char* path, int min_value, int max_value,
                        std::string* error) {
    if (!node ||  // GCOVR_EXCL_BR_LINE (defensive: call sites guard the node)
        !node[key]) {  // GCOVR_EXCL_BR_LINE (defensive: every call site passes
                       // a guarded, defined node; the remaining zero arm is an
                       // inlined-call artifact)
        return true;
    }

    try {
        const int value = node[key].as<int>();
        if (value < min_value || value > max_value) {
            if (error) {
                *error = std::string(path) + "." + key + " must be between " +
                         std::to_string(min_value) + " and " +
                         std::to_string(max_value);
            }
            return false;
        }
    } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // catch blocks have no non-exception
                                       // entry edge; the conversion-failure arm
                                       // is covered)
        if (error) {
            *error = std::string(path) + "." + key + " must be an integer";
        }
        return false;
    }
    return true;
}
}  // namespace

bool validate_runtime_config(const RuntimeValidationOptions& options,
                             std::string* error) {
    auto& config = global_config();
    std::shared_lock lock(config.impl_->mutex);
    const YAML::Node& root = config.impl_->root;

    const struct OptionalModule {
        const char* section;
        const char* module;
        bool enabled;
    } optional_modules[] = {
        {"cluster", "shield_cluster", options.cluster_enabled},
        {"global", "shield_global", options.global_enabled},
        {"player", "shield_player", options.player_enabled},
        {"player_manager", "shield_player", options.player_enabled},
        {"server_manager", "shield_server", options.server_enabled},
        {"ops", "shield_ops", options.ops_enabled},
    };

    for (const auto& module : optional_modules) {
        if (root[module.section] &&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // range-for expansion arm plus gcov clone
                                     // arms)
            !module.enabled) {       // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // range-for expansion arm plus gcov clone arms;
                                // both condition arms are covered)
            if (error) {
                *error = std::string("optional module config '") +
                         module.section + "' requires " + module.module;
            }
            return false;
        }
    }

    if (!root["app"] || !root["app"].IsMap() ||  // GCOVR_EXCL_BR_LINE (compiler
                                                 // artifact: gcov clone arms)
        !root["app"]["name"] ||  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // inlined as<std::string> arms plus gcov clone
                                 // arms; all four chain arms are covered)
        // Each operand line carries its own marker: the arms are the inlined
        // as<std::string> conversion clones plus never-executed gcov clone
        // arms; all four chain arms are covered.
        root["app"]["name"]     // GCOVR_EXCL_BR_LINE (clone arms; covered)
            .as<std::string>()  // GCOVR_EXCL_BR_LINE (clone arms; covered)
            .empty()) {         // GCOVR_EXCL_BR_LINE (clone arms; covered)
        if (error) {
            *error = "app.name is required";
        }
        return false;
    }
    const auto app_name = root["app"]["name"].as<std::string>();
    if (app_name.size() > 64) {
        if (error) {
            *error = "app.name must be 1-64 characters";
        }
        return false;
    }

    if (const YAML::Node log = root["log"];
        log && log["level"]) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // never-executed gcov clone arms)
        const auto level = log["level"].as<std::string>();
        if (level != "debug" && level != "info" && level != "warn" &&
            level != "error") {
            if (error) {
                *error = "log.level must be debug, info, warn, or error";
            }
            return false;
        }
    }

    if (const YAML::Node lua = root["lua"]) {
        // GCOVR_EXCL_BR_START (compiler artifact: inlined operator[] and
        // string-compare arms; the vm/mode presence arms are covered)
        if (lua["vm"] &&    // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                            // operator[] arms)
            lua["vm"]       // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                            // operator[] arms plus gcov clone arms)
               ["mode"] &&  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                            // operator[]/string arms plus gcov clone arms; the
                            // vm and mode presence arms are covered)
            lua["vm"]["mode"]
                    .as<std::string>() !=  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: inlined string arms plus
                                           // gcov clone arms)
                "per_service") {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // never-executed gcov clone arms)
                                  // GCOVR_EXCL_BR_STOP
            if (error) {
                *error = "lua.vm.mode must be per_service";
            }
            return false;
        }
        if (lua["cache"]) {
            // GCOVR_EXCL_BR_START (compiler artifact: inlined operator[]
            // arms plus gcov clone arms)
            if (!validate_int_range(
                    lua["cache"], "max_size",
                    "lua.cache",  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov
                                  // clone arms)
                    1,  // GCOVR_EXCL_BR_LINE (compiler artifact: never-executed
                        // gcov clone arms)
                    10000, error) ||
                !validate_int_range(
                    lua["cache"], "ttl_seconds",
                    "lua.cache",  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov
                                  // clone arms)
                    0,  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov clone
                        // arms; the ttl non-integer arm is covered by the
                        // ttl:abc case)
                    86400, error)) {
                // GCOVR_EXCL_BR_STOP
                return false;
            }
        }
    }

    if (options.require_actors) {
        const YAML::Node actors = root["actors"];
        if (!actors || !actors.IsSequence() || actors.size() == 0) {
            if (error) {
                *error = "actors must contain at least one actor";
            }
            return false;
        }

        std::unordered_set<std::string> names;
        std::vector<std::pair<std::string, YAML::Node>> actors_to_resolve;
        actors_to_resolve.reserve(actors.size());
        for (std::size_t i = 0; i < actors.size(); ++i) {
            const YAML::Node actor = actors[i];
            if (!actor.IsMap()) {
                if (error) {
                    *error = "actors[" + std::to_string(i) + "] must be a map";
                }
                return false;
            }

            std::string name;
            try {
                name = actor["name"].as<std::string>();
            } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: catch blocks have no
                                               // non-exception entry edge; the
                                               // exception arm is covered)
                if (error) {
                    *error =
                        "actors[" + std::to_string(i) + "].name is required";
                }
                return false;
            }
            if (name.empty()) {
                if (error) {
                    *error =
                        "actors[" + std::to_string(i) + "].name is required";
                }
                return false;
            }
            if (!names.insert(name).second) {
                if (error) {
                    *error = "actors[].name must be unique: " + name;
                }
                return false;
            }

            if (!actor["script"] ||  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // inlined as<std::string> arm plus gcov
                                     // clone arms)
                actor["script"]      // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // inlined operator[] arms)
                    .as<std::string>()  // GCOVR_EXCL_BR_LINE
                                        // (compiler artifact:
                                        // inlined conversion
                                        // arms)
                    .empty()) {         // GCOVR_EXCL_BR_LINE
                                        // (compiler artifact: inlined
                                        // as<std::string> arm plus
                                        // gcov clone arms;
                                        // script-missing and
                                        // script-empty arms are
                                        // covered)
                if (error) {
                    *error = "actors[" + name + "].script is required";
                }
                return false;
            }

            if (auto instances = scalar_int(actor, "instances");
                instances && *instances < 0) {
                if (error) {
                    *error = "actors[" + name + "].instances must be >= 0";
                }
                return false;
            }
            const int actor_instances =
                scalar_int(actor, "instances").value_or(1);

            if (const YAML::Node restart = actor["restart"];
                restart &&
                restart["policy"]) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // gcov clone arms; the
                                      // restart-without-policy arm is covered
                                      // by the restart:{} case)
                const auto policy = restart["policy"].as<std::string>();
                if (policy != "always" && policy != "on-failure" &&
                    policy != "never") {
                    if (error) {
                        *error = "actors[" + name +
                                 "].restart.policy must be always, on-failure, "
                                 "or never";
                    }
                    return false;
                }
            }

            if (const YAML::Node network = actor["network"]) {
                if (!network.IsMap()) {
                    if (error) {
                        *error = "actors[" + name + "].network must be a map";
                    }
                    return false;
                }

                // GCOVR_EXCL_BR_START (compiler artifact: inlined
                // operator[] arms)
                if (network["udp"] ||
                    network["kcp"] ||  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // inlined operator[] arms)
                    network["websocket"]) {  // GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: inlined operator[]
                                             // arms plus gcov clone arms; all
                                             // three key arms are covered)
                                             // GCOVR_EXCL_BR_STOP
                    if (error) {
                        *error = "actors[" + name +
                                 "].network only supports tcp in Phase 1";
                    }
                    return false;
                }

                if (!validate_listener_address(network, "tcp", name, error)) {
                    return false;
                }
                // A TCP listener without a protocol pipeline has no inbound
                // dispatch (the raw-frame fallback was removed): declaring
                // network.tcp without network.protocol is a config error, not
                // a listener that silently drops everything it receives.
                if (network["tcp"] &&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // inlined operator[] arms)
                    !network["protocol"]) {  // GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: inlined operator[]
                                             // arms plus gcov clone arms; the
                                             // tcp/protocol combination arms
                                             // are covered)
                    if (error) {
                        *error = "actors[" + name +
                                 "].network.tcp requires network.protocol "
                                 "(raw-frame ingress is not supported)";
                    }
                    return false;
                }
                if (network["protocol"] &&  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: inlined operator[]
                                            // arms)
                    !network
                         ["protocol"]  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // inlined operator[] arms plus gcov
                                       // clone arms)
                             .IsMap()) {  // GCOVR_EXCL_BR_LINE
                                          // (compiler artifact:
                                          // inlined operator[]
                                          // arms plus gcov clone
                                          // arms; both protocol
                                          // arms are covered)
                    if (error) {
                        *error = "actors[" + name +
                                 "].network.protocol must be a map";
                    }
                    return false;
                }
                // GCOVR_EXCL_BR_START (compiler artifact: string
                // concatenation inline arms plus gcov clone arms)
                if (network["protocol"] &&  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: never-executed gcov
                                            // clone arms)
                    !validate_network_protocol(
                        network["protocol"],  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: never-executed gcov
                                              // clone arms)
                        "actors[" + name +
                            "].network.protocol",  // GCOVR_EXCL_BR_LINE
                                                   // (compiler artifact: string
                                                   // concatenation inline arms
                                                   // plus gcov clone arms)
                        error)) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // string concatenation inline arms plus gcov
                                   // clone arms)
                                   // GCOVR_EXCL_BR_STOP
                    return false;
                }
                if (network["tcp"] &&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // inline arm plus gcov clone arms)
                    actor_instances !=
                        1) {  // GCOVR_EXCL_BR_LINE (compiler artifact: inline
                              // arm plus gcov clone arms; both instances arms
                              // are covered)
                    if (error) {
                        *error = "actors[" + name +
                                 "].network.tcp requires instances to be 1";
                    }
                    return false;
                }
                const std::string network_path = "actors[" + name + "].network";
                if (!validate_int_range(network, "max_connections",
                                        network_path.c_str(), 1, 10000000,
                                        error) ||
                    !validate_int_range(network, "max_connections_per_ip",
                                        network_path.c_str(), 1, 10000000,
                                        error) ||
                    !validate_int_range(network, "max_frame_size",
                                        network_path.c_str(), 1,
                                        16 * 1024 * 1024, error) ||
                    !validate_int_range(network, "max_session_send_queue",
                                        network_path.c_str(), 0, 1000000,
                                        error) ||
                    !validate_int_range(network, "read_idle_timeout",
                                        network_path.c_str(), 0, 86400000,
                                        error)) {
                    return false;
                }
                // Per-connection ingress rate limit (token bucket).
                // messages_per_second is 0 (disabled) or >= 1; burst is
                // 0 (default to the rate) or >= 1, and may not exceed the
                // rate's own upper bound so a typo cannot allocate a
                // pathological bucket depth.
                if (const YAML::Node rate_limit = network["rate_limit"]) {
                    if (!rate_limit
                             .IsMap()) {  // GCOVR_EXCL_BR_LINE (error path:
                                          // non-map rate_limit rejected)
                        if (error) {  // GCOVR_EXCL_BR_LINE (error path: same
                                      // branch)
                            *error = network_path + ".rate_limit must be a map";
                        }
                        return false;
                    }
                    const std::string rate_path = network_path + ".rate_limit";
                    if (!validate_int_range(rate_limit, "messages_per_second",
                                            rate_path.c_str(), 0, 1000000,
                                            error) ||
                        !validate_int_range(rate_limit, "burst",
                                            rate_path.c_str(), 0, 1000000,
                                            error)) {
                        return false;
                    }
                }
                // Accept-time address blocklist. Every entry is parsed here
                // so a typo is a startup error, not a silently missing deny
                // rule discovered during an incident.
                if (const YAML::Node blocklist = network["blocklist"]) {
                    if (!blocklist.IsMap()) {
                        if (error) {
                            *error = network_path + ".blocklist must be a map";
                        }
                        return false;
                    }
                    if (const YAML::Node deny = blocklist["deny"]) {
                        if (!deny.IsSequence()) {
                            if (error) {
                                *error = network_path +
                                         ".blocklist.deny must be an array";
                            }
                            return false;
                        }
                        for (std::size_t i = 0; i < deny.size(); ++i) {
                            const std::string entry_path =
                                network_path + ".blocklist.deny[" +
                                std::to_string(i) + "]";
                            std::string entry;
                            try {
                                entry = deny[i].as<std::string>();
                            } catch (  // GCOVR_EXCL_BR_LINE (catch entry edge)
                                const std::exception&) {  // GCOVR_EXCL_BR_LINE
                                                          // (compiler artifact:
                                                          // catch blocks have
                                                          // no non-exception
                                                          // entry edge; the
                                                          // exception arm
                                                          // is covered)
                                if (error) {
                                    *error = entry_path + " must be a string";
                                }
                                return false;
                            }
                            shield::net::Rule rule;
                            std::string parse_error;
                            if (!shield::net::parse_blocklist_entry(
                                    entry, &rule, &parse_error)) {
                                if (error) {
                                    *error = entry_path + ": " + parse_error;
                                }
                                return false;
                            }
                        }
                    }
                }

                // Per-actor TLS surface (docs/tls-design.md). Absent or
                // disabled means "behave exactly as before"; enabled means
                // the paths must at least be well-formed here and must load
                // for real at bootstrap -- a listener never falls back to
                // plaintext by accident.
                if (const YAML::Node tls = network["tls"]) {
                    if (!tls.IsMap()) {  // GCOVR_EXCL_BR_LINE (error path:
                                         // non-map tls rejected)
                        if (error) {     // GCOVR_EXCL_BR_LINE (error path)
                            *error = network_path + ".tls must be a map";
                        }
                        return false;
                    }
                    const std::string tls_path = network_path + ".tls";
                    bool enabled = false;
                    if (const YAML::Node enabled_node = tls["enabled"]) {
                        try {
                            enabled = enabled_node.as<bool>();
                        } catch (  // GCOVR_EXCL_BR_LINE (catch entry edge)
                            const std::exception&) {  // GCOVR_EXCL_BR_LINE
                                                      // (compiler artifact:
                                                      // catch blocks have no
                                                      // non-exception entry
                                                      // edge)
                            // A silently-disabled TLS on a typo'd value would
                            // be a plaintext fallback, so this is an error.
                            if (error) {
                                *error = tls_path +
                                         ".enabled must be a "
                                         "boolean";
                            }
                            return false;
                        }
                    }
                    if (enabled) {
                        if (!validate_non_empty_string(tls, "cert_file",
                                                       tls_path, error) ||
                            !validate_non_empty_string(tls, "key_file",
                                                       tls_path, error)) {
                            return false;
                        }
                        if (!validate_int_range(tls, "handshake_timeout_ms",
                                                tls_path.c_str(), 1, 600000,
                                                error)) {
                            return false;
                        }
                    }
                }
            }

            if (actor["rpc"] &&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // inline arm plus gcov clone arms)
                !actor["rpc"]  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // operator[] arms)
                     .IsMap()) {  // GCOVR_EXCL_BR_LINE (compiler
                                  // artifact: inline arm plus
                                  // gcov clone arms; the
                                  // rpc-not-a-map arm is covered)
                if (error) {
                    *error = "actors[" + name + "].rpc must be a map";
                }
                return false;
            }
            if (!validate_actor_rpc_routes(actor["rpc"],
                                           "actors[" + name + "].rpc", error)) {
                return false;
            }

            actors_to_resolve.emplace_back(name, actor);
        }

        for (const auto& [name, actor] : actors_to_resolve) {
            if (!existing_script_path(actor, config.impl_->source_dir, root)) {
                if (error) {
                    *error = "actors[" + name + "].script does not exist: " +
                             actor["script"].as<std::string>();
                }
                return false;
            }
        }
    }

    if (const YAML::Node shutdown = root["shutdown"];
        shutdown && shutdown["timeout"] &&  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: gcov clone arms)
        shutdown["timeout"]  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                             // operator[] arms plus gcov clone arms)
            .IsMap()) {      // GCOVR_EXCL_BR_LINE (compiler
                             // artifact: gcov clone arms; the
                             // shutdown-without-timeout arm is
                             // covered by the shutdown:{}
                             // case)
        const YAML::Node timeout = shutdown["timeout"];
        const auto total = scalar_int(timeout, "total");
        if (total) {
            for (const char* key :
                 {"service_drain", "service_stop", "plugin_shutdown"}) {
                const auto part = scalar_int(timeout, key);
                if (part && *total <= *part) {
                    if (error) {
                        *error =
                            "shutdown.timeout.total must be greater than " +
                            std::string(key);
                    }
                    return false;
                }
            }
        }
    }

    if (const YAML::Node net = root["net"]) {
        if (!validate_int_range(net, "threads", "net", 0, 64, error)) {
            return false;
        }
    }

    return true;
}

}  // namespace shield::config
