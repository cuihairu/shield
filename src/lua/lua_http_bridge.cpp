// [SHIELD_LUA] Bridge inbound HTTP requests to Lua service handlers
#include "shield/lua/lua_http_bridge.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

namespace shield::lua {

namespace {

// Bound on how long an HTTP io thread waits for the owning service actor to
// run the Lua handler. Mirrors the OpsHttpHandler timeout.
constexpr auto kDispatchTimeout = std::chrono::seconds(2);

shield::net::HttpMethod method_from_string(const std::string& method) {
    if (method == "GET") return shield::net::HttpMethod::GET;
    if (method == "POST") return shield::net::HttpMethod::POST;
    if (method == "PUT") return shield::net::HttpMethod::PUT;
    if (method == "DELETE") return shield::net::HttpMethod::DELETE_;
    if (method == "PATCH") return shield::net::HttpMethod::PATCH;
    return shield::net::HttpMethod::ANY;  // GCOVR_EXCL_LINE (unreachable:
                                          // shield.httpd only registers the
                                          // five verbs above)
}

shield::net::HttpResponse error_response(int status, const std::string& what) {
    shield::net::HttpResponse resp;
    resp.result(static_cast<boost::beast::http::status>(status));
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = nlohmann::json({{"type", "error"}, {"message", what}}).dump();
    resp.prepare_payload();
    return resp;
}

}  // namespace

LuaHttpBridge::LuaHttpBridge(LuaRuntime& runtime, LuaServiceManager& manager)
    : runtime_(runtime), manager_(manager) {}

LuaHttpBridge::~LuaHttpBridge() { detach(); }

void LuaHttpBridge::attach(shield::net::HttpServer& server) {
    {
        std::lock_guard<std::mutex> lock(server_mutex_);
        server_ = &server;
    }
    // Mirror routes registered before attach (service on_init phase).
    for (const auto& route : runtime_.http_routes()) {
        register_on_server(route.method, route.path);
    }
    // Late registrations (running services) flow through the sink.
    runtime_.set_http_route_sink(
        [this](const std::string& method, const std::string& path) {
            register_on_server(method, path);
        });
}

void LuaHttpBridge::detach() {
    runtime_.set_http_route_sink({});
    std::lock_guard<std::mutex> lock(server_mutex_);
    server_ = nullptr;
}

void LuaHttpBridge::register_on_server(const std::string& method,
                                       const std::string& path) {
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (!server_) {
        return;
    }
    server_->route(
        method_from_string(method), path,
        [this](const shield::net::HttpRequest& req) { return handle(req); });
}

shield::net::HttpResponse LuaHttpBridge::handle(
    const shield::net::HttpRequest& req) {
    const std::string method(req.method_string());
    std::string target(req.target());
    std::string query;
    const auto qpos = target.find('?');
    if (qpos != std::string::npos) {
        query = target.substr(qpos + 1);
        target = target.substr(0, qpos);
    }

    std::vector<std::pair<std::string, std::string>> params;
    auto route = runtime_.find_http_route(method, target, &params);
    if (!route) {
        return error_response(404, "route not found: " + method + " " + target);
    }

    // Snapshot request data as JSON so nothing Lua-bound crosses threads.
    nlohmann::json params_json = nlohmann::json::object();
    for (const auto& [k, v] : params) {
        params_json[k] = v;
    }
    nlohmann::json headers_json = nlohmann::json::object();
    for (const auto& field : req.base()) {
        headers_json[std::string(field.name_string())] =
            std::string(field.value());
    }
    nlohmann::json request_json = {
        {"method", method},        {"path", target},
        {"query", query},          {"params", params_json},
        {"headers", headers_json}, {"body", req.body()},
    };

    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();

    // The forked task runs on the registering service's actor thread, so
    // the handler executes on its VM's serialized dispatch path.
    const uint64_t task_id = manager_.enqueue_forked_task(
        route->service_id, [this, route = *route, request_json, promise]() {
            nlohmann::json desc;
            std::string error;
            if (runtime_.call_http_handler(route, request_json, desc, &error)) {
                promise->set_value(std::move(desc));
            } else {
                promise->set_value(
                    nlohmann::json({{"lua_error", std::move(error)}}));
            }
        });

    if (task_id == 0) {
        return error_response(503,
                              "service is not running: " + route->service_id);
    }

    if (future.wait_for(kDispatchTimeout) != std::future_status::ready) {
        return error_response(504, "handler timeout: " + route->service_id);
    }

    nlohmann::json desc;
    try {
        desc = future.get();
    } catch (const std::exception& e) {
        return error_response(500, std::string(e.what()));
    }

    if (desc.contains("lua_error")) {
        return error_response(500, desc["lua_error"].get<std::string>());
    }

    shield::net::HttpResponse resp;
    const int status = desc.value("status", 200);
    resp.result(static_cast<boost::beast::http::status>(status));
    if (desc.contains("headers")) {
        for (auto it = desc["headers"].begin(); it != desc["headers"].end();
             ++it) {
            resp.set(it.key(), it.value().get<std::string>());
        }
    }
    resp.body() = desc.value("body", std::string());
    if (status != 204 &&
        resp.base().find(boost::beast::http::field::content_type) ==
            resp.base().end()) {
        resp.set(
            boost::beast::http::field::content_type,
            desc.value("json_body", false) ? "application/json" : "text/plain");
    }
    resp.prepare_payload();
    return resp;
}

}  // namespace shield::lua
