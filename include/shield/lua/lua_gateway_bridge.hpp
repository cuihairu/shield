// [SHIELD_LUA] Gateway bridge between shield_net sessions and Lua handlers
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace shield::transport {
struct DispatchResult;
}

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
/// - On protocol decode-local packet: routes decoded business message through
///   on_client_message(session_handle, message)
/// - On disconnect: calls gateway_service.on_disconnect(session_handle, reason)
///
/// ForwardRaw/Drop protocol actions stay in the C++ data plane and are not
/// exposed to Lua.
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

    /// @brief Handle a routed packet from a TCP session.
    ///
    /// Only DecodeLocal packets are forwarded into Lua, and they are bridged
    /// as on_client_message payloads. ForwardRaw/Drop remain in C++.
    void on_packet(std::shared_ptr<shield::net::Session> session,
                   const shield::transport::DispatchResult& packet);

    /// @brief Handle a TCP session disconnection.
    void on_disconnect(std::shared_ptr<shield::net::Session> session,
                       std::string reason);

private:
    LuaServiceManager& manager_;
    std::string gateway_service_name_;
};

}  // namespace shield::lua
