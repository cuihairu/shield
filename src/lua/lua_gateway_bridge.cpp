// [SHIELD_LUA] Gateway bridge implementation
#include "shield/lua/lua_gateway_bridge.hpp"

#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"

#include <nlohmann/json.hpp>

namespace shield::lua {

LuaGatewayBridge::LuaGatewayBridge(LuaServiceManager& manager,
                                     std::string gateway_service_name)
    : manager_(manager),
      gateway_service_name_(std::move(gateway_service_name)) {}

void LuaGatewayBridge::on_connect(std::shared_ptr<shield::net::Session> session) {
    if (!session) return;

    // Build session info as a JSON object that the Lua handler can read.
    // The Lua gateway_service.lua expects a table with id and remote_addr fields.
    nlohmann::json session_info = {
        {"id", std::to_string(session->id())},
        {"remote_addr", session->remote_addr().to_string()}
    };

    manager_.send(gateway_service_name_, "on_connect",
                  nlohmann::json::array({session_info}));
}

void LuaGatewayBridge::on_message(std::shared_ptr<shield::net::Session> session,
                                    const std::string& payload) {
    if (!session) return;

    nlohmann::json session_info = {
        {"id", std::to_string(session->id())},
        {"remote_addr", session->remote_addr().to_string()}
    };

    manager_.send(gateway_service_name_, "on_client_message",
                  nlohmann::json::array({session_info, payload}));
}

void LuaGatewayBridge::on_disconnect(std::shared_ptr<shield::net::Session> session,
                                       std::string reason) {
    if (!session) return;

    nlohmann::json session_info = {
        {"id", std::to_string(session->id())},
        {"remote_addr", session->remote_addr().to_string()}
    };

    manager_.send(gateway_service_name_, "on_disconnect",
                  nlohmann::json::array({session_info, std::move(reason)}));
}

}  // namespace shield::lua
