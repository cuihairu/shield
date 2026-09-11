// [SHIELD_LUA] Gateway bridge implementation
#include "shield/lua/lua_gateway_bridge.hpp"

#include <nlohmann/json.hpp>

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/protocol.hpp"

namespace shield::lua {

namespace {

/// Trusted client identity for a session in its current binding state. This
/// is the JSON marker form: the Lua API layer materializes it into a
/// read-only ClientContext userdata on delivery.
nlohmann::json client_context_marker(const std::string& gateway_name,
                                     net::SessionId session_id,
                                     const net::SessionBinding& binding) {
    return ClientContextData{gateway_name, session_id, binding.epoch,
                             binding.player_id, binding.protocol_profile_id}
        .to_json();
}

}  // namespace

LuaGatewayBridge::LuaGatewayBridge(
    LuaServiceManager& manager, std::string auth_service_name,
    std::shared_ptr<GatewaySessionRegistry> registry)
    : manager_(manager),
      auth_service_name_(std::move(auth_service_name)),
      registry_(std::move(registry)) {}

void LuaGatewayBridge::on_connect(
    std::shared_ptr<shield::net::Session> session) {
    if (!session) return;

    // Install the initial single-target binding: the listener's auth entry
    // service, no player identity, epoch 0.
    net::SessionBinding initial;
    initial.target_service = auth_service_name_;
    initial.gateway_name = auth_service_name_;
    session->reset_binding(initial);

    if (registry_) {
        registry_->add(session, initial);
    }

    const nlohmann::json client_context =
        client_context_marker(auth_service_name_, session->id(), initial);

    // Notify the auth service of new connection. send_system routes through
    // the target's CAF actor (fire-and-forget anon_send), so it is safe to
    // call directly from the network thread — no fork-task wrapper needed.
    std::string error;
    if (!manager_.send_system(auth_service_name_, "on_connect",
                              nlohmann::json::array({client_context}),
                              &error)) {
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

    const net::SessionBinding binding = session->binding();

    // Check auth requirement
    if (route->requires_auth && binding.player_id.empty()) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Rejected unauthenticated access to route: " +
                                    std::to_string(route_id));
        return;
    }

    // 3. Single target: forward to the session's bound target service. The
    // RPC descriptor set owns binding -> owner mapping inside the target VM;
    // the gateway never derives a service name from the route or the body.
    const std::string target = binding.target_service;

    if (target.empty()) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Session has no target service, route_id: " +
                                    std::to_string(route_id));
        return;
    }

    // 4. Build ClientIngress with the live binding state
    ClientIngress ingress;
    ingress.gateway_service_name = auth_service_name_;  // response route back
    ingress.session_id = session->id();
    ingress.session_epoch = binding.epoch;
    ingress.player_id = binding.player_id;
    ingress.route_id = route_id;
    ingress.protocol_profile_id = binding.protocol_profile_id;

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

    if (registry_) {
        registry_->remove(session->id());
    }

    // Notify the current target service of disconnection. The session is on
    // its way out; no binding invalidation is needed beyond dropping the
    // registry entry (egress rejects unknown sessions).
    net::SessionBinding binding = session->binding();
    std::string target = binding.target_service;
    if (target.empty()) {
        target = auth_service_name_;
    }
    const nlohmann::json client_context =
        client_context_marker(auth_service_name_, session->id(), binding);

    std::string error;
    if (!manager_.send_system(target, "on_disconnect",
                              nlohmann::json::array({client_context, reason}),
                              &error)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Failed to queue on_disconnect: " + error);
    }
}

void LuaGatewayBridge::send_client_ingress(const std::string& target,
                                           const ClientIngress& ingress) {
    // Transitional flattening ([M3] replaces this with the typed CAF
    // ClientIngress message). The target Lua service receives:
    //   on_client_message(route_id, client_context, body_str, message)
    // where client_context materializes as a read-only ClientContext
    // userdata from the __shield_client_ref marker. body_bytes is passed as
    // a raw string; decoded_message is the codec plugin's canonical JSON
    // message (a Lua table), or nil when no codec plugin decoded the body.
    const ClientContextData context{
        ingress.gateway_service_name, ingress.session_id, ingress.session_epoch,
        ingress.player_id, ingress.protocol_profile_id};

    // body_bytes as raw string for Lua
    std::string body_str(ingress.body_bytes.begin(), ingress.body_bytes.end());

    // Decoded canonical message, or JSON null (Lua nil) when absent.
    const nlohmann::json decoded_message = ingress.decoded_message.has_value()
                                               ? *ingress.decoded_message
                                               : nlohmann::json(nullptr);

    std::string error;
    if (!manager_.send_system(
            target, "on_client_message",
            nlohmann::json::array({ingress.route_id, context.to_json(),
                                   body_str, decoded_message}),
            &error, ingress.session_id, ingress.session_epoch)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Failed to queue ClientIngress: " + error);
    }
}

}  // namespace shield::lua
