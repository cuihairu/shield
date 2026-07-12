// [SHIELD_LUA] Gateway bridge between shield_net sessions and Lua handlers
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shield::transport {
struct DispatchResult;
struct RouteEntry;
}  // namespace shield::transport

namespace shield::net {
class Session;
}

namespace shield::lua {

class LuaServiceManager;

/// @brief ClientIngress message passed from Gateway to target Service VM.
///
/// This is a runtime internal message, not a Lua API. The target Service VM
/// uses route_id to select a cached handler, then decodes body_bytes by
/// the RPC's request_schema.
struct ClientIngress {
    std::string gateway_service_name;  // for response routing back
    uint64_t session_id = 0;
    uint32_t session_epoch = 0;
    std::string player_id;             // empty before auth
    std::string protocol_profile_id;
    uint32_t route_id = 0;             // from wire header
    std::vector<uint8_t> body_bytes;   // pure business data, pass-through
};

/// @brief Bridge that routes TCP session events to the session's target
/// service (AuthService pre-login, PlayerService post-login).
///
/// Routing model:
/// - Gateway reads header route_id for validation only (direction + auth)
/// - All validated messages go to session.target_service
/// - Target VM uses route_id -> cached handler dispatch
/// - Gateway does NOT parse route_id for target service selection
/// - Gateway does NOT know about room/scene/map
class LuaGatewayBridge {
public:
    LuaGatewayBridge(LuaServiceManager& manager,
                     std::string auth_service_name);

    /// @brief Handle a new TCP session connection.
    void on_connect(std::shared_ptr<shield::net::Session> session);

    /// @brief Handle a routed packet from a TCP session.
    ///
    /// Validates route_id (existence, direction, auth), then sends
    /// ClientIngress to session.target_service.
    void on_packet(std::shared_ptr<shield::net::Session> session,
                   const shield::transport::DispatchResult& packet);

    /// @brief Handle a TCP session disconnection.
    void on_disconnect(std::shared_ptr<shield::net::Session> session,
                       std::string reason);

private:
    /// @brief Send ClientIngress to target service via LuaServiceManager.
    void send_client_ingress(const std::string& target,
                             const ClientIngress& ingress);

    LuaServiceManager& manager_;
    std::string auth_service_name_;  // pre-login target
};

}  // namespace shield::lua
