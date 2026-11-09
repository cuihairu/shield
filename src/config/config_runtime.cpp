// [SHIELD_CONFIG] Runtime accessors: the parsed runtime_actors /
// runtime_net_threads views consumed by the bootstrap phases.
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

namespace {
bool scalar_bool_default(const YAML::Node& node, const char* key,
                         bool fallback) {
    if (!node ||  // GCOVR_EXCL_BR_LINE (defensive: call sites guard the node)
        !node[key]) {  // GCOVR_EXCL_BR_LINE (defensive: every call site passes
                       // a guarded, defined node; the remaining zero arm is an
                       // inlined-call artifact)
        return fallback;
    }
    try {
        return node[key].as<bool>();
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch entry edge)
        const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // catch blocks have no non-exception entry
                                  // edge; the exception arm is covered)
        return fallback;
    }
}
}  // namespace

std::vector<RuntimeActorConfig> runtime_actors() {
    auto& config = global_config();
    std::shared_lock lock(config.impl_->mutex);

    std::vector<RuntimeActorConfig> result;
    const YAML::Node actors = config.impl_->root["actors"];
    if (!actors || !actors.IsSequence()) {
        return result;
    }

    for (const auto& actor : actors) {
        RuntimeActorConfig item;
        item.name = actor["name"].as<std::string>();
        item.script = actor["script"].as<std::string>();
        item.source_dir = config.impl_->source_dir;
        item.instances = scalar_int(actor, "instances").value_or(1);
        item.required = scalar_bool_default(actor, "required", true);

        if (actor["options"]) {
            item.options_json = yaml_to_json(actor["options"]).dump();
        }
        if (const YAML::Node network = actor["network"]) {
            if (network["tcp"]) {
                item.network_tcp = network["tcp"].as<std::string>();
            }
            item.max_connections = static_cast<size_t>(
                scalar_int(network, "max_connections").value_or(0));
            item.max_connections_per_ip = static_cast<size_t>(
                scalar_int(network, "max_connections_per_ip").value_or(0));
            item.max_frame_size = static_cast<size_t>(
                scalar_int(network, "max_frame_size").value_or(0));
            item.max_session_send_queue = static_cast<size_t>(
                scalar_int(network, "max_session_send_queue").value_or(0));
            if (network["read_idle_timeout"]) {
                item.read_idle_timeout_ms = static_cast<uint32_t>(
                    scalar_int(network, "read_idle_timeout").value_or(0));
            }
            if (const YAML::Node rate_limit = network["rate_limit"]) {
                item.rate_limit_per_second = static_cast<uint32_t>(
                    scalar_int(rate_limit, "messages_per_second").value_or(0));
                item.rate_limit_burst = static_cast<uint32_t>(
                    scalar_int(rate_limit, "burst").value_or(0));
            }
            if (const YAML::Node blocklist = network["blocklist"]) {
                if (const YAML::Node deny = blocklist["deny"]) {
                    item.blocklist_deny =
                        yaml_to_json(deny).get<std::vector<std::string>>();
                }
            }
            if (network["protocol"]) {
                item.network_protocol_enabled = true;
                item.network_protocol_json =
                    yaml_to_json(network["protocol"]).dump();
            }
            if (const YAML::Node tls = network["tls"]) {
                item.network_tls_enabled =
                    scalar_bool_default(tls, "enabled", false);
                if (item.network_tls_enabled) {
                    // Validation guarantees presence and non-emptiness for an
                    // enabled block; handshake_timeout_ms falls back to the
                    // documented default when omitted.
                    item.network_tls_cert_file =
                        tls["cert_file"].as<std::string>();
                    item.network_tls_key_file =
                        tls["key_file"].as<std::string>();
                    item.network_tls_handshake_timeout_ms =
                        static_cast<uint32_t>(
                            scalar_int(tls, "handshake_timeout_ms")
                                .value_or(10000));
                }
            }
        }
        if (const YAML::Node rpc = actor["rpc"]) {
            // Intermediate node check: chaining ["routes"] directly onto an
            // undefined rpc node throws yaml-cpp's InvalidNode.
            if (const YAML::Node routes = rpc["routes"]) {
                item.rpc_routes_json = yaml_to_json(routes).dump();
            }
        }

        result.push_back(std::move(item));
    }

    return result;
}

size_t runtime_net_threads() {
    auto& config = global_config();
    std::shared_lock lock(config.impl_->mutex);
    const YAML::Node root = config.impl_->root;
    if (const YAML::Node net = root["net"]) {
        if (net["threads"]) {
            try {
                const auto value = net["threads"].as<int>();
                if (value < 0) return 0;
                return static_cast<size_t>(value);
            } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                               // artifact: catch blocks have no
                                               // non-exception entry edge; the
                                               // exception arm is covered)
            }
        }
    }
    return 0;
}

}  // namespace shield::config
