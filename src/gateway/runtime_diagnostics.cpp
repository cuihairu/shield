#include "shield/gateway/runtime_diagnostics.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <sstream>

#include "shield/actor/distributed_actor_system.hpp"
#include "shield/log/logger.hpp"
#include "shield/service/service_api.hpp"

#ifdef SHIELD_ENABLE_PROMETHEUS
#include "shield/metrics/prometheus_service.hpp"
#endif

namespace shield::gateway {

std::string RuntimeDiagnostics::health_json() {
    nlohmann::json j;
    j["status"] = "ok";
    j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return j.dump();
}

std::string RuntimeDiagnostics::health_detailed_json() {
    nlohmann::json j;
    j["status"] = "ok";
    j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    // Services
    auto service_names = service::list_services();
    j["services"]["count"] = service_names.size();
    j["services"]["list"] = service_names;

    // Node info
    j["node"]["id"] = service::self_node_id();

    return j.dump();
}

std::string RuntimeDiagnostics::runtime_status_json(
    actor::DistributedActorSystem& dist_system) {
    nlohmann::json j;

    // Node
    j["node"]["id"] = service::self_node_id();
    j["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    // Services
    auto service_names = service::list_services();
    j["services"]["registered"] = service_names;

    // Actors by type
    auto gateway_actors =
        dist_system.find_actors_by_type(actor::ActorType::GATEWAY, true, false);
    auto logic_actors =
        dist_system.find_actors_by_type(actor::ActorType::LOGIC, true, false);
    auto custom_actors =
        dist_system.find_actors_by_type(actor::ActorType::CUSTOM, true, false);

    j["actors"]["gateway_count"] = gateway_actors.size();
    j["actors"]["logic_count"] = logic_actors.size();
    j["actors"]["custom_count"] = custom_actors.size();
    j["actors"]["total"] =
        gateway_actors.size() + logic_actors.size() + custom_actors.size();

    // Actor details
    nlohmann::json actors_arr = nlohmann::json::array();
    auto add_actors = [&](auto& actors) {
        for (auto& ra : actors) {
            nlohmann::json a;
            a["name"] = ra.metadata.name;
            a["type"] = ra.metadata.type_to_string();
            a["node"] = ra.metadata.node_id;
            a["local"] = ra.is_local;
            actors_arr.push_back(a);
        }
    };
    add_actors(gateway_actors);
    add_actors(logic_actors);
    add_actors(custom_actors);
    j["actors"]["details"] = actors_arr;

    // Version
    j["version"] = "0.1.0";

    return j.dump();
}

std::string RuntimeDiagnostics::config_reload_info_json() {
    nlohmann::json j;

    j["reload"]["supported"] = true;
    j["reload"]["trigger"] = "file modification (FileWatcher)";
    j["reload"]["scope"] = nlohmann::json::array({
        "gateway listener configuration",
        "log levels (global, per-module)",
        "lua script hot-reload (on_message handlers)",
    });
    j["reload"]["not_supported"] = nlohmann::json::array({
        "actor lifecycle changes",
        "service registration changes",
        "network port changes (requires restart)",
    });

    return j.dump();
}

}  // namespace shield::gateway
