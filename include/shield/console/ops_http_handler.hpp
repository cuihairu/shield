// [SHIELD_CONSOLE] HTTP ops endpoints implementation
#pragma once

#include <memory>
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
/// - GET /ops/status - Service/plugin/cluster status
/// - GET /ops/services - List all Lua services
/// - GET /ops/plugins - List all plugins
/// - GET /ops/config - Show config
/// - POST /ops/eval - Execute Lua code in sandbox
class OpsHttpHandler {
public:
    OpsHttpHandler(shield::lua::LuaServiceManager& lua_mgr,
                   shield::lua::LuaRuntime& lua_rt);
    ~OpsHttpHandler() = default;

    /// @brief Register all /ops/* routes on the given HTTP server
    void register_routes(shield::net::HttpServer& server);

private:
    // Route handlers
    shield::net::HttpResponse handle_status(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_services(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_plugins(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_config(
        const shield::net::HttpRequest& req);
    shield::net::HttpResponse handle_eval(const shield::net::HttpRequest& req);

    // Helper: create JSON response
    static shield::net::HttpResponse make_json_response(
        int status_code, const nlohmann::json& data);
    static shield::net::HttpResponse make_error_response(
        int status_code, const std::string& message);

    shield::lua::LuaServiceManager& lua_mgr_;
    shield::lua::LuaRuntime& lua_rt_;
};

}  // namespace shield::console
