// [SHIELD_LUA] Gateway bridge between shield_net sessions and Lua handlers
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace shield::net {
class Session;
}

namespace shield::lua {

class LuaServiceManager;

/// @brief Bridge that routes TCP session events to Lua gateway service
/// handlers.
///
/// When a gateway service is configured, the bridge:
/// - On connect: calls gateway_service.on_connect(session_handle)
/// - On message: calls gateway_service.on_client_message(session_handle,
/// payload)
/// - On disconnect: calls gateway_service.on_disconnect(session_handle, reason)
class LuaGatewayBridge {
public:
    /// @brief Create a gateway bridge.
    /// @param manager The Lua service manager
    /// @param gateway_service_name The name of the gateway Lua service
    LuaGatewayBridge(LuaServiceManager& manager,
                     std::string gateway_service_name);

    /// @brief Handle a new TCP session connection.
    void on_connect(std::shared_ptr<shield::net::Session> session);

    /// @brief Handle a message from a TCP session.
    void on_message(std::shared_ptr<shield::net::Session> session,
                    const std::string& payload);

    /// @brief Handle a TCP session disconnection.
    void on_disconnect(std::shared_ptr<shield::net::Session> session,
                       std::string reason);

private:
    LuaServiceManager& manager_;
    std::string gateway_service_name_;
};

}  // namespace shield::lua
