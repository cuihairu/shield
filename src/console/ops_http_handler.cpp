// [SHIELD_CONSOLE] HTTP ops endpoints implementation
#include "shield/console/ops_http_handler.hpp"

#include <future>
#include <nlohmann/json.hpp>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/plugin/plugin_host.hpp"

#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif

namespace shield::console {

OpsHttpHandler::OpsHttpHandler(shield::lua::LuaServiceManager& lua_mgr,
                               shield::lua::LuaRuntime& lua_rt)
    : lua_mgr_(lua_mgr), lua_rt_(lua_rt) {}

void OpsHttpHandler::register_routes(shield::net::HttpServer& server) {
    server.get("/ops/status",
               [this](const auto& req) { return handle_status(req); });
    server.get("/ops/services",
               [this](const auto& req) { return handle_services(req); });
    server.get("/ops/plugins",
               [this](const auto& req) { return handle_plugins(req); });
    server.get("/ops/config",
               [this](const auto& req) { return handle_config(req); });
    server.post("/ops/eval",
                [this](const auto& req) { return handle_eval(req); });
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
            auto nodes = cm->nodes();
            nlohmann::json cluster;
            cluster["node_id"] = cm->node_id();
            cluster["node_epoch"] = cm->node_epoch();
            cluster["nodes"] = nlohmann::json::array();
            for (const auto& n : nodes) {
                cluster["nodes"].push_back(
                    {{"node_id", n.node_id},
                     {"address", n.address},
                     {"state", shield::cluster::node_state_name(n.state)},
                     {"epoch", n.epoch}});
            }
            data["cluster"] = cluster;
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
        return make_json_response(200, {{"type", "result"},
                                        {"data", "Use ?key=<key> to query"}}));
    }

    auto value = shield::config::get(target, "");
    if (value.empty()) {
        return make_json_response(200, {{"type", "result"}, {"data", nullptr}});
    }
    return make_json_response(200, {{"type", "result"}, {"data", value}});
}

shield::net::HttpResponse OpsHttpHandler::handle_eval(
    const shield::net::HttpRequest& req) {
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

    // Execute in sandbox
    auto vm = lua_rt_.create_vm();
    lua_rt_.register_api(vm);

    nlohmann::json result;
    std::string error;
    bool ok = lua_rt_.exec_lua(vm, code, &result, &error);

    lua_rt_.destroy_vm(vm);

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
}

shield::net::HttpResponse OpsHttpHandler::make_error_response(
    int status_code, const std::string& message) {
    return make_json_response(status_code,
                              {{"type", "error"}, {"message", message}});
}

}  // namespace shield::console
