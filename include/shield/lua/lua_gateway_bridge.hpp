// [SHIELD_LUA] Gateway bridge between shield_net sessions and Lua handlers
#pragma once

#include <memory>
#include <string>

#include "shield/core/service_message.hpp"

namespace shield::transport {
struct DispatchResult;
struct RouteEntry;
}  // namespace shield::transport

namespace shield::net {
class Session;
}

namespace shield::lua {

class LuaServiceManager;
class GatewaySessionRegistry;

/// @brief Bridge that routes TCP session events to the session's target
/// service (the listener's auth entry service pre-login, the bound target
/// after shield.client.bind).
///
/// Routing model:
/// - The bridge registers/removes sessions in the listener's
///   GatewaySessionRegistry; the gateway actor owns binding changes
/// - Single target: validated ingress goes to the session's bound
///   target_service, never to a per-route service
/// - Validated ingress travels as a typed ClientIngress CAF message; session
///   lifecycle (attach / detach / kick) travels as typed ClientControlMessage
/// - The client identity travels inside ClientContextData and materializes
///   into a read-only ClientContext userdata on the Lua side
class LuaGatewayBridge {
public:
    LuaGatewayBridge(LuaServiceManager& manager, std::string auth_service_name,
                     std::shared_ptr<GatewaySessionRegistry> registry);

    /// @brief Handle a new TCP session connection.
    ///
    /// Installs the initial single-target binding (the listener's auth entry
    /// service, epoch 0) and notifies it with ClientControlMessage::Bound.
    void on_connect(std::shared_ptr<shield::net::Session> session);

    /// @brief Handle a routed packet from a TCP session.
    ///
    /// Validates route_id (existence, direction, auth), then sends a typed
    /// ClientIngress to the session's bound target.
    void on_packet(std::shared_ptr<shield::net::Session> session,
                   const shield::transport::DispatchResult& packet);

    /// @brief Handle a TCP session disconnection.
    ///
    /// Notifies the current target with ClientControlMessage::Disconnected
    /// (a kicked session is notified by the gateway actor with Unbound while
    /// its binding is still live, so the empty-binding case here is skipped).
    void on_disconnect(std::shared_ptr<shield::net::Session> session,
                       std::string reason);

private:
    LuaServiceManager& manager_;
    std::string auth_service_name_;  // pre-login target
    std::shared_ptr<GatewaySessionRegistry> registry_;
};

}  // namespace shield::lua
