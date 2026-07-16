// [SHIELD_LUA] Gateway bridge implementation
#include "shield/lua/lua_gateway_bridge.hpp"

#include <nlohmann/json.hpp>

#include "shield/log/logger.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/protocol.hpp"

namespace shield::lua {

LuaGatewayBridge::LuaGatewayBridge(LuaServiceManager& manager,
                                   std::string auth_service_name)
    : manager_(manager), auth_service_name_(std::move(auth_service_name)) {}

void LuaGatewayBridge::on_connect(
    std::shared_ptr<shield::net::Session> session) {
    if (!session) return;

    // Set default target to AuthService for pre-login
    session->set_target_service(auth_service_name_);
    session->set_epoch(0);

    const auto session_info = make_session_handle_json(session);

    // Notify the auth service of new connection. send_system routes through
    // the target's CAF actor (fire-and-forget anon_send), so it is safe to
    // call directly from the network thread — no fork-task wrapper needed.
    std::string error;
    if (!manager_.send_system(auth_service_name_, "on_connect",
                              nlohmann::json::array({session_info}), &error)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Failed to queue on_connect: " + error);
    }
}

void LuaGatewayBridge::on_packet(
    std::shared_ptr<shield::net::Session> session,
    const shield::transport::DispatchResult& packet) {
    if (!session) return;
    if (!packet.ok() || packet.should_drop() || packet.should_forward_raw()) {
        return;
    }

    // 1. Get route_id from wire header
    uint32_t route_id = packet.packet.route_id;

    // 2. Validate route via Gateway route table
    const auto* route = packet.route;
    if (!route) {
        // Unknown route_id, reject
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log,
                           "Unknown route_id: " + std::to_string(route_id));
        return;
    }

    // Check direction: client can only send ClientToServer or Bidirectional
    if (route->direction == shield::transport::RouteDirection::ServerToClient) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log,
                           "Rejected server_to_client route from client: " +
                               std::to_string(route_id));
        return;
    }

    // Check auth requirement
    if (route->requires_auth && session->player_id().empty()) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Rejected unauthenticated access to route: " +
                                    std::to_string(route_id));
        return;
    }

    // 3. Get session target service
    std::string target = session->target_service();
    if (target.empty()) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Session has no target service, route_id: " +
                                    std::to_string(route_id));
        return;
    }

    // 4. Build ClientIngress
    ClientIngress ingress;
    ingress.gateway_service_name = auth_service_name_;  // for response routing
    ingress.session_id = session->id();
    ingress.session_epoch = session->epoch();
    ingress.player_id = session->player_id();
    ingress.route_id = route_id;

    // body_bytes: pass through raw bytes (Gateway does not decode body)
    if (packet.decoded_body.has_value()) {
        ingress.body_bytes = packet.decoded_body->bytes;
    } else {
        ingress.body_bytes = std::vector<uint8_t>(packet.packet.body.begin(),
                                                  packet.packet.body.end());
    }

    // 5. Send to session.target via LuaServiceManager
    send_client_ingress(target, ingress);
}

void LuaGatewayBridge::on_disconnect(
    std::shared_ptr<shield::net::Session> session, std::string reason) {
    if (!session) return;

    const auto session_info = make_session_handle_json(session);

    // Notify current target service of disconnection
    std::string target = session->target_service();
    if (target.empty()) {
        target = auth_service_name_;
    }

    std::string error;
    if (!manager_.send_system(target, "on_disconnect",
                              nlohmann::json::array({session_info, reason}),
                              &error)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Failed to queue on_disconnect: " + error);
    }
}

void LuaGatewayBridge::send_client_ingress(const std::string& target,
                                           const ClientIngress& ingress) {
    // Serialize ClientIngress to JSON for Lua consumption
    // Target service receives: on_client_message(route_id, client_context,
    // body)
    //
    // For now, use the existing send_system mechanism.
    // The target Lua service will receive:
    //   on_client_message(route_id, {session_id, player_id, ...}, body_bytes)
    //
    // body_bytes is passed as raw string; the target VM decodes it
    // according to the RPC's request_schema.

    nlohmann::json client_context = {
        {"session_id", ingress.session_id},
        {"session_epoch", ingress.session_epoch},
        {"player_id", ingress.player_id},
        {"gateway_service", ingress.gateway_service_name},
    };

    // body_bytes as raw string for Lua
    std::string body_str(ingress.body_bytes.begin(), ingress.body_bytes.end());

    // send_system routes the on_client_message call through the target's CAF
    // actor directly (no fork-task wrapper). The target Lua service receives:
    //   on_client_message(route_id, client_context, body_str)
    std::string error;
    if (!manager_.send_system(
            target, "on_client_message",
            nlohmann::json::array({ingress.route_id, client_context, body_str}),
            &error)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Failed to queue ClientIngress: " + error);
    }
}

}  // namespace shield::lua
