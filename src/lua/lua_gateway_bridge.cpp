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

    // Initialize routing context
    auto& routing = session->routing_context();
    routing.gateway_address = auth_service_name_;
    routing.session_id = std::to_string(session->id());
    routing.session_epoch = 0;

    // Bind AuthService as the default route
    shield::net::ServiceAddress auth_addr;
    auth_addr.service_id = auth_service_name_;
    auth_addr.service_type = "auth";
    auth_addr.epoch = 0;
    session->bind_service("auth", std::move(auth_addr));

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

    // 3. Get target service using routing context
    // First try to get from route's logical service name, then fallback to
    // session's target service
    std::string target;
    if (route->logical_service_name) {
        const auto* addr = session->get_service(*route->logical_service_name);
        if (addr) {
            target = addr->service_id;
        }
    }

    // Fallback to session's target service if no route-specific binding
    if (target.empty()) {
        target = session->target_service();
    }

    if (target.empty()) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Session has no target service, route_id: " +
                                    std::to_string(route_id));
        return;
    }

    // 4. Build ClientIngress with epoch validation
    ClientIngress ingress;
    ingress.gateway_service_name = auth_service_name_;  // for response routing
    ingress.session_id = session->id();
    ingress.session_epoch = session->epoch();
    ingress.player_id = session->player_id();
    ingress.route_id = route_id;
    ingress.protocol_profile_id = session->protocol_profile_id();

    // body_bytes: pass through raw bytes (Gateway does not decode body)
    if (packet.decoded_body.has_value()) {
        ingress.body_bytes = packet.decoded_body->bytes;
        // decoded_message: when the pipeline's codec plugin decoded the
        // payload, forward the canonical JSON message alongside the raw
        // bytes so Lua services can consume it directly as a table.
        if (packet.decoded_body->has_message()) {
            ingress.decoded_message = packet.decoded_body->message;
        }
    } else {
        ingress.body_bytes = std::vector<uint8_t>(packet.packet.body.begin(),
                                                  packet.packet.body.end());
    }

    // 5. Send to target service via LuaServiceManager
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

    // Clear all routes on disconnect
    session->routing_context().clear_routes();

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
    // Target service receives:
    //   on_client_message(route_id, client_context, body, message)
    //
    // For now, use the existing send_system mechanism.
    // The target Lua service will receive:
    //   on_client_message(route_id, {session_id, player_id, ...}, body_bytes,
    //                     decoded_message)
    //
    // body_bytes is passed as raw string; the target VM decodes it
    // according to the RPC's request_schema. decoded_message is the codec
    // plugin's canonical JSON message (a Lua table), or nil when no codec
    // plugin decoded the body.

    nlohmann::json client_context = {
        {"session_id", ingress.session_id},
        {"session_epoch", ingress.session_epoch},
        {"player_id", ingress.player_id},
        {"gateway_service", ingress.gateway_service_name},
        {"protocol_profile_id", ingress.protocol_profile_id},
    };

    // body_bytes as raw string for Lua
    std::string body_str(ingress.body_bytes.begin(), ingress.body_bytes.end());

    // Decoded canonical message, or JSON null (Lua nil) when absent.
    const nlohmann::json decoded_message = ingress.decoded_message.has_value()
                                               ? *ingress.decoded_message
                                               : nlohmann::json(nullptr);

    // send_system routes the on_client_message call through the target's CAF
    // actor directly (no fork-task wrapper). The target Lua service receives:
    //   on_client_message(route_id, client_context, body_str, message)
    std::string error;
    if (!manager_.send_system(
            target, "on_client_message",
            nlohmann::json::array(
                {ingress.route_id, client_context, body_str, decoded_message}),
            &error, ingress.session_id, ingress.session_epoch)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Failed to queue ClientIngress: " + error);
    }
}

}  // namespace shield::lua
