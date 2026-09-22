// [SHIELD_CONSOLE] HTTP ops endpoints implementation
#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "shield/console/lua_commands.hpp"
#include "shield/console/root_commands.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/http_server.hpp"

namespace shield::console {

/// @brief HTTP endpoints for ops/monitoring
///
/// Provides REST API endpoints that mirror the console commands:
/// - GET /ops/health - Lightweight health probe (no Lua actor round trip)
/// - GET /ops/status - Service/plugin/cluster status
/// - GET /ops/metrics - Prometheus text format metrics
/// - GET /ops/services - List all Lua services
/// - GET /ops/plugins - List all plugins
/// - GET /ops/config - Show config
/// - POST /ops/eval - Execute Lua code; opt-in and token-gated
/// - POST /ops/profile - Sampling hotspot profiler; opt-in and token-gated
///
/// /ops/eval is a remote-code-entry point, so it is registered only when
/// http.eval_enabled=true AND a non-empty http.eval_token is configured.
/// Requests must carry "Authorization: Bearer <token>"; the code runs in a
/// restricted VM (no os.execute/io/require, see LuaRuntime::restrict_vm).
///
/// /ops/profile (action: start|stop|report|status) reads code locations and
/// hit counts only. Same opt-in discipline: http.profile_enabled=true plus
/// a non-empty http.profile_token; a start cooldown
/// (http.profile_cooldown_seconds, default 10) rate-limits sessions on top
/// of the manager's single-active-session arbitration.
class OpsHttpHandler {
public:
    OpsHttpHandler(shield::lua::LuaServiceManager& lua_mgr,
                   shield::lua::LuaRuntime& lua_rt);
    ~OpsHttpHandler() = default;

    /// @brief Register all /ops/* routes on the given HTTP server
    void register_routes(shield::net::HttpServer& server);

private:
    // Route handlers
    shield::net::HttpResponse handle_health(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_status(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_metrics(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_services(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_service_detail(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_plugins(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_config(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_eval(const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_profile(
        const shield::net::HttpRequest& req);

    // Helper: constant-time Bearer token comparison for the eval gate
    static bool token_matches(const std::string& provided,
                              const std::string& expected);

    // Helper: create JSON response
    static shield::net::HttpResponse make_json_response(
        int status_code, const nlohmann::json& data);
    static shield::net::HttpResponse make_error_response(
        int status_code, const std::string& message);

    shield::lua::LuaServiceManager& lua_mgr_;
    shield::lua::LuaRuntime& lua_rt_;
    std::string eval_token_;

    // /ops/profile state. The report future is created at start and
    // consumed at stop/report; the mutex covers its hand-off between
    // concurrent requests (session arbitration itself is the manager's).
    std::string profile_token_;
    std::mutex profile_mu_;
    std::shared_future<nlohmann::json> profile_report_;
    std::chrono::steady_clock::time_point last_profile_start_ =
        std::chrono::steady_clock::time_point::min();
    bool profile_started_once_ = false;
};

}  // namespace shield::console
