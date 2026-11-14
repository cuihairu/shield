// tools/bots/plan.cpp - YAML plan parsing and validation.
#include "plan.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <stdexcept>

namespace shield::bots {

namespace {

std::uint32_t require_route(const YAML::Node& node, const char* key,
                            const std::string& what) {
    if (!node[key]) {
        throw std::runtime_error(what + ": missing " + key);
    }
    const auto value = node[key].as<int>();
    if (value <= 0 || value > 0xFFFF) {
        throw std::runtime_error(what + ": " + key + " must be in 1..65535");
    }
    return static_cast<std::uint32_t>(value);
}

// yaml-cpp has no built-in nlohmann::json converter; plan bodies only need
// scalars, maps, and lists, so a small recursive walk is enough.
nlohmann::json yaml_to_json(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Map: {
            nlohmann::json object = nlohmann::json::object();
            for (const auto& item : node) {
                object[item.first.as<std::string>()] =
                    yaml_to_json(item.second);
            }
            return object;
        }
        case YAML::NodeType::Sequence: {
            nlohmann::json array = nlohmann::json::array();
            for (const auto& item : node) {
                array.push_back(yaml_to_json(item));
            }
            return array;
        }
        case YAML::NodeType::Scalar: {
            // Numbers first (x: 10 must reach Lua as a number), then booleans
            // (JSON spelling only — yaml-cpp would also fold yes/no/on/off),
            // then fall back to string.
            try {
                return nlohmann::json(node.as<long long>());
            } catch (const YAML::BadConversion&) {
            }
            try {
                return nlohmann::json(node.as<double>());
            } catch (const YAML::BadConversion&) {
            }
            const auto text = node.Scalar();
            if (text == "true") {
                return nlohmann::json(true);
            }
            if (text == "false") {
                return nlohmann::json(false);
            }
            return nlohmann::json(text);
        }
        default:
            return nlohmann::json();
    }
}

}  // namespace

int LoadPlan::connections_at(double second) const {
    if (ramp.empty()) {
        return connections;
    }
    int live = 0;
    for (const auto& stage : ramp) {
        if (stage.at_second <= second) {
            live = stage.connections;
        }
    }
    return live;
}

int LoadPlan::peak_connections() const {
    if (ramp.empty()) {
        return connections;
    }
    int peak = 0;
    for (const auto& stage : ramp) {
        peak = std::max(peak, stage.connections);
    }
    return peak;
}

LoadPlan load_plan(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot load plan " + path + ": " + e.what());
    }
    if (!root.IsMap()) {
        throw std::runtime_error("plan root must be a map");
    }

    LoadPlan plan;
    if (root["name"]) {
        plan.name = root["name"].as<std::string>();
    }
    if (!root["target"]) {
        throw std::runtime_error("plan: missing target");
    }
    plan.target = root["target"].as<std::string>();

    if (root["duration_seconds"]) {
        plan.duration_seconds = root["duration_seconds"].as<double>();
    }
    if (plan.duration_seconds <= 0) {
        throw std::runtime_error("plan: duration_seconds must be > 0");
    }
    if (root["connections"]) {
        plan.connections = root["connections"].as<int>();
    }
    if (root["requests_per_second"]) {
        plan.requests_per_second = root["requests_per_second"].as<double>();
    }
    if (plan.requests_per_second <= 0) {
        throw std::runtime_error("plan: requests_per_second must be > 0");
    }
    if (root["timeout_ms"]) {
        plan.timeout_ms = root["timeout_ms"].as<int>();
    }
    if (root["reconnect_max_attempts"]) {
        plan.reconnect_max_attempts = root["reconnect_max_attempts"].as<int>();
    }
    if (root["login"]) {
        const auto login = root["login"];
        if (login["route_id"]) {
            plan.login_route_id =
                require_route(login, "route_id", "plan.login");
        }
        if (login["reply_route_id"]) {
            plan.login_reply_route_id =
                require_route(login, "reply_route_id", "plan.login");
        }
        if (login["body"] && login["body"]["player_id"]) {
            plan.login_player_prefix =
                login["body"]["player_id"].as<std::string>();
        }
    }

    if (root["ramp"]) {
        for (const auto& stage : root["ramp"]) {
            if (!stage["at_second"] || !stage["connections"]) {
                throw std::runtime_error(
                    "plan.ramp: each stage needs at_second + connections");
            }
            RampStage parsed;
            parsed.at_second = stage["at_second"].as<double>();
            parsed.connections = stage["connections"].as<int>();
            if (parsed.connections < 0) {
                throw std::runtime_error("plan.ramp: connections must be >= 0");
            }
            plan.ramp.push_back(parsed);
        }
    }

    if (!root["work_routes"] || !root["work_routes"].IsSequence() ||
        root["work_routes"].size() == 0) {
        throw std::runtime_error("plan: work_routes must be a non-empty list");
    }
    for (const auto& entry : root["work_routes"]) {
        WorkRoute route;
        route.route_id = require_route(entry, "route_id", "plan.work_routes");
        if (entry["reply_route_id"]) {
            route.reply_route_id =
                require_route(entry, "reply_route_id",
                              "plan.work_routes[route " +
                                  std::to_string(route.route_id) + "]");
        } else {
            // Reply-less routes (pure fire-and-forget) are allowed; no
            // latency is measured for them.
            route.reply_route_id = 0;
        }
        if (entry["weight"]) {
            route.weight = entry["weight"].as<int>();
        }
        if (route.weight < 0) {
            throw std::runtime_error("plan.work_routes: weight must be >= 0");
        }
        if (entry["body"]) {
            route.body = yaml_to_json(entry["body"]);
        }
        plan.work_routes.push_back(route);
    }

    int total_weight = 0;
    for (const auto& route : plan.work_routes) {
        total_weight += route.weight;
    }
    if (total_weight == 0) {
        throw std::runtime_error(
            "plan: at least one work route needs weight > 0");
    }
    return plan;
}

}  // namespace shield::bots
