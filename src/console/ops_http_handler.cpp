// [SHIELD_CONSOLE] HTTP ops endpoints implementation
#include "shield/console/ops_http_handler.hpp"

#include <chrono>
#include <future>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/plugin_host.hpp"

#ifdef SHIELD_ENABLE_CLUSTER
#include "cluster_status.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#endif

#ifdef SHIELD_ENABLE_SERVER
#include "server_status.hpp"
#include "shield/server/server_manager.hpp"
#endif

#ifdef SHIELD_ENABLE_GLOBAL
#include "global_status.hpp"
#include "shield/global/global_manager.hpp"
#endif

namespace {

/// Process start reference for uptime-based health and metrics. First use
/// in this translation unit pins the process start (good enough for a
/// health probe; the optional server module has the authoritative clock).
const std::chrono::steady_clock::time_point kProcessStart =
    std::chrono::steady_clock::now();

double process_uptime_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         kProcessStart)
        .count();
}

/// Prometheus label-value escaping (backslash, quote, newline).
std::string prom_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// Appends one HELP/TYPE header pair plus a single sample line.
void prom_emit(std::string& out, const std::string& name,
               const std::string& type, const std::string& help,
               const std::string& labels, double value) {
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    out += name;
    if (!labels.empty()) {
        out += "{" + labels + "}";
    }
    out += ' ';
    if (value == static_cast<long long>(value)) {
        out += std::to_string(static_cast<long long>(value));
    } else {
        out += std::to_string(value);
    }
    out += '\n';
}

/// Grouped variant: one HELP/TYPE header pair, then one sample per label
/// set (labels must already be escaped).
void prom_emit_group(
    std::string& out, const std::string& name, const std::string& type,
    const std::string& help,
    const std::vector<std::pair<std::string, double>>& samples) {
    if (samples.empty()) {
        return;
    }
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    for (const auto& [labels, value] : samples) {
        out += name;
        if (!labels.empty()) {
            out += "{" + labels + "}";
        }
        out += ' ';
        if (value == static_cast<long long>(value)) {
            out += std::to_string(static_cast<long long>(value));
        } else {
            out += std::to_string(value);
        }
        out += '\n';
    }
}

}  // namespace

namespace shield::console {

OpsHttpHandler::OpsHttpHandler(shield::lua::LuaServiceManager& lua_mgr,
                               shield::lua::LuaRuntime& lua_rt)
    : lua_mgr_(lua_mgr), lua_rt_(lua_rt) {}

void OpsHttpHandler::register_routes(shield::net::HttpServer& server) {
    server.get("/ops/health",
               [this](const auto& req) { return handle_health(req); });
    server.get("/ops/status",
               [this](const auto& req) { return handle_status(req); });
    server.get("/ops/metrics",
               [this](const auto& req) { return handle_metrics(req); });
    server.get("/ops/services",
               [this](const auto& req) { return handle_services(req); });
    // ":name" pattern matches only when the exact "/ops/services" route
    // missed, so list vs. detail never shadow each other.
    server.get("/ops/services/:name",
               [this](const auto& req) { return handle_service_detail(req); });
    server.get("/ops/plugins",
               [this](const auto& req) { return handle_plugins(req); });
    server.get("/ops/config",
               [this](const auto& req) { return handle_config(req); });

    // /ops/eval is a remote-code-entry point: opt-in (http.eval_enabled),
    // token-gated (http.eval_token), and served in a restricted VM. The
    // route simply stays unregistered otherwise, so the endpoint 404s.
    if (shield::config::get("http.eval_enabled", "false") != "true") {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_INFO(log,
                        "/ops/eval disabled (opt in via http.eval_enabled=true "
                        "+ http.eval_token)");
        return;
    }
    eval_token_ = shield::config::get("http.eval_token", "");
    if (eval_token_.empty()) {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_ERROR(log,
                         "/ops/eval NOT registered: http.eval_enabled=true "
                         "requires a non-empty http.eval_token");
        return;
    }
    server.post("/ops/eval",
                [this](const auto& req) { return handle_eval(req); });
}

bool OpsHttpHandler::token_matches(const std::string& provided,
                                   const std::string& expected) {
    // A length mismatch only leaks the length, never token content; equal
    // lengths compare over every byte so timing reveals nothing.
    if (provided.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(provided[i]) ^
                static_cast<unsigned char>(expected[i]);
    }
    return diff == 0;
}

shield::net::HttpResponse OpsHttpHandler::handle_health(
    const shield::net::HttpRequest& req) {
    // Lightweight probe: process-local reads only, never a Lua actor round
    // trip, so it answers even when the actor mesh is wedged.
    nlohmann::json checks;
    checks["core"] = {{"status", "ok"},
                      {"uptime_seconds", process_uptime_seconds()}};

    // Plugins: a required instance that did not reach "started" degrades
    // the process (absent optional instances are fine).
    {
        std::size_t started = 0;
        std::size_t required_down = 0;
        for (const auto& inst :
             shield::plugin::global_host().list_instances()) {
            if (inst.state == "started") {
                ++started;
            } else if (inst.required) {
                ++required_down;
            }
        }
        checks["plugins"] = {{"status", required_down == 0 ? "ok" : "degraded"},
                             {"started", started},
                             {"required_down", required_down}};
    }

#ifdef SHIELD_ENABLE_SERVER
    if (auto* sm = shield::server::ServerManager::global()) {
        const char* state = shield::server::server_state_name(sm->state());
        // Only "running" counts as healthy; starting/maintenance/shutdown
        // all mean the process is not serving normally right now.
        const bool ok = sm->state() == shield::server::ServerState::kRunning;
        checks["server"] = {{"status", ok ? "ok" : "degraded"},
                            {"state", state}};
    }
#endif

#ifdef SHIELD_ENABLE_CLUSTER
    if (auto* cm = shield::cluster::global_cluster_manager()) {
        std::size_t online = 0;
        std::size_t down = 0;
        for (const auto& n : cm->nodes()) {
            const auto state = n.state;
            if (state == shield::cluster::NodeState::Online) {
                ++online;
            } else if (state == shield::cluster::NodeState::Offline ||
                       state == shield::cluster::NodeState::Removed) {
                ++down;
            }
        }
        checks["cluster"] = {{"status", down == 0 ? "ok" : "degraded"},
                             {"nodes_online", online},
                             {"nodes_down", down}};
    }
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    // Same convention as server/cluster above: a GLOBAL-capable build may
    // still run as a non-global node, so absence omits the check rather
    // than degrading it.
    if (shield::global::GlobalManager::global() != nullptr) {
        checks["global"] = {{"status", "ok"}};
    }
#endif

    // Overall: degraded when any check degraded (P0 keeps HTTP 200 and
    // carries the verdict in the body; a 503 mapping can come later).
    std::string status = "ok";
    for (const auto& [name, check] : checks.items()) {
        (void)name;
        if (check.value("status", "ok") != "ok") {
            status = "degraded";
            break;
        }
    }
    nlohmann::json data = {{"status", status},
                           {"uptime", process_uptime_seconds()},
                           {"checks", std::move(checks)}};
    return make_json_response(200, {{"type", "result"}, {"data", data}});
}

shield::net::HttpResponse OpsHttpHandler::handle_metrics(
    const shield::net::HttpRequest& req) {
    std::string out;
    prom_emit(out, "shield_uptime_seconds", "gauge",
              "Process uptime in seconds", "", process_uptime_seconds());

    // Plugins grouped by lifecycle state.
    {
        std::map<std::string, double> by_state;
        for (const auto& inst :
             shield::plugin::global_host().list_instances()) {
            by_state[inst.state] += 1.0;
        }
        std::vector<std::pair<std::string, double>> samples;
        for (const auto& [state, count] : by_state) {
            samples.emplace_back("state=\"" + prom_escape(state) + "\"", count);
        }
        prom_emit_group(out, "shield_plugin_instances", "gauge",
                        "Plugin instances by lifecycle state", samples);
    }

    // Service count: the one metric that needs the actor mesh; omitted on
    // timeout so a wedged mesh never wedges the scrape.
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
            auto names = mgr.list_services();
            promise->set_value(nlohmann::json(names));
        });
        if (future.wait_for(std::chrono::milliseconds(500)) ==
            std::future_status::ready) {
            const double count = static_cast<double>(future.get().size());
            prom_emit(out, "shield_services", "gauge",
                      "Registered Lua services", "", count);
        }
    }

#ifdef SHIELD_ENABLE_SERVER
    if (auto* sm = shield::server::ServerManager::global()) {
        prom_emit(
            out, "shield_server_state", "gauge",
            "Server state machine (1 for the current state)",
            "state=\"" +
                prom_escape(shield::server::server_state_name(sm->state())) +
                "\"",
            1.0);
    }
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    if (auto* gm = shield::global::GlobalManager::global()) {
        prom_emit(out, "shield_global_data_keys", "gauge", "Global data keys",
                  "", static_cast<double>(gm->data_size()));
        prom_emit(out, "shield_global_cache_entries", "gauge",
                  "Local cache entries", "",
                  static_cast<double>(gm->cache_size()));
        prom_emit(out, "shield_global_cache_hits_total", "counter",
                  "Local cache hits", "",
                  static_cast<double>(gm->cache_hits()));
        prom_emit(out, "shield_global_cache_misses_total", "counter",
                  "Local cache misses", "",
                  static_cast<double>(gm->cache_misses()));
        prom_emit_group(
            out, "shield_global_queues", "gauge", "Queues by family",
            {{"type=\"normal\"", static_cast<double>(gm->queue_count())},
             {"type=\"delay\"", static_cast<double>(gm->delay_queue_count())},
             {"type=\"priority\"",
              static_cast<double>(gm->priority_queue_count())},
             {"type=\"broadcast\"",
              static_cast<double>(gm->broadcast_queue_count())},
             {"type=\"reliable\"",
              static_cast<double>(gm->reliable_queue_count())}});
        prom_emit_group(
            out, "shield_global_locks", "gauge", "Live locks by kind",
            {{"kind=\"mutex\"",
              static_cast<double>(gm->mutex_registry_size("mutex"))},
             {"kind=\"spinlock\"",
              static_cast<double>(gm->mutex_registry_size("spinlock"))},
             {"kind=\"rwlock\"", static_cast<double>(gm->rwlock_count())}});
        prom_emit(out, "shield_global_rank_boards", "gauge",
                  "Leaderboard boards", "",
                  static_cast<double>(gm->rank_board_count()));
        prom_emit(out, "shield_global_rank_members", "gauge",
                  "Leaderboard members across boards", "",
                  static_cast<double>(gm->rank_total_members()));
        prom_emit(out, "shield_global_scheduler_tasks", "gauge",
                  "Registered scheduler tasks", "",
                  static_cast<double>(gm->sched_list().size()));
        prom_emit(out, "shield_global_scheduler_active", "gauge",
                  "Active (not paused/done) scheduler tasks", "",
                  static_cast<double>(gm->sched_active_count()));
    }
#endif

#ifdef SHIELD_ENABLE_CLUSTER
    if (auto* cm = shield::cluster::global_cluster_manager()) {
        std::map<std::string, double> nodes_by_state;
        for (const auto& n : cm->nodes()) {
            nodes_by_state[shield::cluster::node_state_name(n.state)] += 1.0;
        }
        std::vector<std::pair<std::string, double>> samples;
        for (const auto& [state, count] : nodes_by_state) {
            samples.emplace_back("state=\"" + prom_escape(state) + "\"", count);
        }
        prom_emit_group(out, "shield_cluster_nodes", "gauge",
                        "Cluster nodes by state", samples);
        if (auto* ct = shield::cluster::global_cluster_transport()) {
            const auto stats = ct->stats();
            prom_emit(out, "shield_cluster_transport_connections", "gauge",
                      "Live cluster transport connections", "",
                      static_cast<double>(stats.live_connections));
            prom_emit(out, "shield_cluster_transport_reconnects_total",
                      "counter", "Cluster transport reconnects", "",
                      static_cast<double>(stats.reconnects));
            prom_emit_group(
                out, "shield_cluster_transport_messages_total", "counter",
                "Cluster transport messages",
                {{"direction=\"tx\"", static_cast<double>(stats.tx_messages)},
                 {"direction=\"rx\"", static_cast<double>(stats.rx_messages)}});
            prom_emit_group(
                out, "shield_cluster_transport_heartbeats_total", "counter",
                "Cluster transport heartbeats",
                {{"direction=\"tx\"", static_cast<double>(stats.tx_heartbeats)},
                 {"direction=\"rx\"",
                  static_cast<double>(stats.rx_heartbeats)}});
        }
    }
#endif

    shield::net::HttpResponse resp;
    resp.result(boost::beast::http::status::ok);
    resp.set(boost::beast::http::field::content_type,
             "text/plain; version=0.0.4; charset=utf-8");
    resp.body() = std::move(out);
    resp.prepare_payload();
    return resp;
}

shield::net::HttpResponse OpsHttpHandler::handle_status(
    const shield::net::HttpRequest& req) {
    nlohmann::json data;

    // Services
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
            auto names = mgr.list_services();
            promise->set_value(nlohmann::json(names));
        });
        if (future.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready) {
            data["services"] = future.get();
        } else {
            data["services"] = "timeout";
        }
    }

    // Plugins
    {
        auto& host = shield::plugin::global_host();
        auto instances = host.list_instances();
        nlohmann::json plugins = nlohmann::json::array();
        for (const auto& inst : instances) {
            plugins.push_back({{"id", inst.id},
                               {"package", inst.package},
                               {"state", inst.state},
                               {"required", inst.required}});
        }
        data["plugins"] = plugins;
    }

    // Cluster
#ifdef SHIELD_ENABLE_CLUSTER
    {
        auto* cm = shield::cluster::global_cluster_manager();
        if (cm) {
            data["cluster"] = build_cluster_status_json();
        }
    }
#endif

    // Server state machine (read-only snapshot)
#ifdef SHIELD_ENABLE_SERVER
    {
        auto* sm = shield::server::ServerManager::global();
        if (sm) {
            data["server"] = build_server_status_json();
        }
    }
#endif

    // Global capability store (read-only snapshot)
#ifdef SHIELD_ENABLE_GLOBAL
    {
        nlohmann::json global = build_global_status_json();
        if (!global.is_null()) {
            data["global"] = std::move(global);
        }
    }
#endif

    return make_json_response(200, {{"type", "result"}, {"data", data}});
}

shield::net::HttpResponse OpsHttpHandler::handle_services(
    const shield::net::HttpRequest& req) {
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
        auto names = mgr.list_services();
        promise->set_value(nlohmann::json(names));
    });
    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return make_json_response(200,
                                  {{"type", "result"}, {"data", future.get()}});
    }
    return make_error_response(504, "timeout querying services");
}

shield::net::HttpResponse OpsHttpHandler::handle_service_detail(
    const shield::net::HttpRequest& req) {
    // Pull ":name" out of /ops/services/<name> (query string stripped).
    std::string target(req.target());
    const auto query_pos = target.find('?');
    if (query_pos != std::string::npos) {
        target.resize(query_pos);
    }
    constexpr char kPrefix[] = "/ops/services/";
    if (target.rfind(kPrefix, 0) != 0 || target.size() <= sizeof(kPrefix) - 1) {
        return make_error_response(404, "missing service name");
    }
    const std::string name = target.substr(sizeof(kPrefix) - 1);

    auto promise =
        std::make_shared<std::promise<std::optional<nlohmann::json>>>();
    auto future = promise->get_future();
    lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise, name]() {
        promise->set_value(mgr.service_detail(name));
    });
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        return make_error_response(504, "timeout querying service");
    }
    auto detail = future.get();
    if (!detail) {
        return make_error_response(404, "service not found: " + name);
    }
    return make_json_response(200, {{"type", "result"}, {"data", *detail}});
}

shield::net::HttpResponse OpsHttpHandler::handle_plugins(
    const shield::net::HttpRequest& req) {
    auto& host = shield::plugin::global_host();
    auto instances = host.list_instances();
    nlohmann::json plugins = nlohmann::json::array();
    for (const auto& inst : instances) {
        plugins.push_back({{"id", inst.id},
                           {"package", inst.package},
                           {"state", inst.state},
                           {"required", inst.required}});
    }
    return make_json_response(200, {{"type", "result"}, {"data", plugins}});
}

shield::net::HttpResponse OpsHttpHandler::handle_config(
    const shield::net::HttpRequest& req) {
    // Parse optional key from query string
    std::string target;
    auto query = req.target();
    auto qpos = query.find('?');
    if (qpos != std::string::npos) {
        auto qs = query.substr(qpos + 1);
        auto kpos = qs.find("key=");
        if (kpos != std::string::npos) {
            target = qs.substr(kpos + 4);
            // Strip any trailing &
            auto amp = target.find('&');
            if (amp != std::string::npos) {
                target = target.substr(0, amp);
            }
        }
    }

    if (target.empty()) {
        // Return all config as JSON
        // Note: This is a simplified version - actual implementation would
        // need to serialize the full config
        return make_json_response(
            200, {{"type", "result"}, {"data", "Use ?key=<key> to query"}});
    }

    auto value = shield::config::get(target, "");
    if (value.empty()) {
        return make_json_response(200, {{"type", "result"}, {"data", nullptr}});
    }
    return make_json_response(200, {{"type", "result"}, {"data", value}});
}

shield::net::HttpResponse OpsHttpHandler::handle_eval(
    const shield::net::HttpRequest& req) {
    // Bearer token gate. Accept only "Authorization: Bearer <token>".
    auto auth_it = req.find(boost::beast::http::field::authorization);
    std::string provided =
        auth_it == req.end() ? "" : std::string(auth_it->value());
    constexpr char kBearerPrefix[] = "Bearer ";
    if (provided.rfind(kBearerPrefix, 0) == 0) {
        provided = provided.substr(sizeof(kBearerPrefix) - 1);
    }
    if (!token_matches(provided, eval_token_)) {
        return make_error_response(401, "unauthorized");
    }

    // Parse JSON body
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception& e) {
        return make_error_response(400, "invalid JSON body");
    }

    if (!body.contains("code") || !body["code"].is_string()) {
        return make_error_response(400, "missing 'code' field");
    }

    std::string code = body["code"].get<std::string>();

    // Execute in a restricted VM: the token gate is the first line of
    // defense, the VM strip (no os.execute/io/require) the second.
    auto vm = lua_rt_.create_vm();
    lua_rt_.register_api(vm);
    lua_rt_.restrict_vm(vm);

    nlohmann::json result;
    std::string error;
    bool ok = lua_rt_.exec_lua(vm, code, &result, &error);

    // VM will be automatically destroyed when shared_ptr goes out of scope
    vm.reset();

    if (ok) {
        return make_json_response(200, {{"type", "result"}, {"data", result}});
    }
    return make_error_response(400, error);
}

shield::net::HttpResponse OpsHttpHandler::make_json_response(
    int status_code, const nlohmann::json& data) {
    shield::net::HttpResponse resp;
    resp.result(static_cast<boost::beast::http::status>(status_code));
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = data.dump(2);
    resp.prepare_payload();
    return resp;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

shield::net::HttpResponse OpsHttpHandler::make_error_response(
    int status_code, const std::string& message) {
    return make_json_response(status_code,
                              {{"type", "error"}, {"message", message}});
}

}  // namespace shield::console
