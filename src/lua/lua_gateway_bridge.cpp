// [SHIELD_LUA] Gateway bridge implementation
#include "shield/lua/lua_gateway_bridge.hpp"

#include <caf/send.hpp>

#include "shield/log/logger.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/protocol.hpp"

namespace shield::lua {

namespace {

/// Trusted client identity for a session in its current binding state.
ClientContextData client_context(const std::string& gateway_name,
                                 net::SessionId session_id,
                                 const net::SessionBinding& binding) {
    return ClientContextData{gateway_name, session_id, binding.epoch,
                             binding.player_id, binding.protocol_profile_id};
}

auto& bridge_log() { return shield::log::get_logger("lua"); }

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

    // Notify the auth service that the session is now bound to it. The
    // target's actor receives a typed control message (fire-and-forget
    // anon_send), so this is safe to call directly from the network thread.
    if (caf::actor target = manager_.service_actor(auth_service_name_);
        target != nullptr) {
        ClientControlMessage bound;
        bound.kind = ClientControlMessage::Kind::Bound;
        bound.context =
            client_context(auth_service_name_, session->id(), initial);
        caf::anon_send(target, std::move(bound));
    } else {
        SHIELD_LOG_WARNING(bridge_log(), "auth service " + auth_service_name_ +
                                             " has no actor for on_connect");
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
        SHIELD_LOG_WARNING(bridge_log(),
                           "Unknown route_id: " + std::to_string(route_id));
        return;
    }

    // Check direction: client can only send ClientToServer or Bidirectional
    if (route->direction == shield::transport::RouteDirection::ServerToClient) {
        SHIELD_LOG_WARNING(bridge_log(),
                           "Rejected server_to_client route from client: " +
                               std::to_string(route_id));
        return;
    }

    const net::SessionBinding binding = session->binding();

    // Check auth requirement
    if (route->requires_auth && binding.player_id.empty()) {
        SHIELD_LOG_WARNING(bridge_log(),
                           "Rejected unauthenticated access to "
                           "route: " +
                               std::to_string(route_id));
        return;
    }

    // 3. Single target: forward to the session's bound target service. The
    // RPC descriptor set owns binding -> owner mapping inside the target VM;
    // the gateway never derives a service name from the route or the body.
    const std::string target = binding.target_service;

    if (target.empty()) {
        SHIELD_LOG_WARNING(bridge_log(),
                           "Session has no target service, "
                           "route_id: " +
                               std::to_string(route_id));
        return;
    }

    // 4. Build the typed ingress with the live binding state and deliver it
    // to the target's actor mailbox.
    ClientIngress ingress;
    ingress.context =
        client_context(auth_service_name_, session->id(), binding);
    ingress.route_id = route_id;

    // body_bytes: pass through raw bytes (Gateway does not decode body); a
    // codec plugin's canonical JSON message rides along as decoded_request.
    if (packet.decoded_body.has_value()) {
        ingress.body_bytes = packet.decoded_body->bytes;
        if (packet.decoded_body->has_message()) {
            ingress.decoded_request = packet.decoded_body->message;
        }
    } else {
        ingress.body_bytes = std::vector<uint8_t>(packet.packet.body.begin(),
                                                  packet.packet.body.end());
    }

    if (caf::actor target_actor = manager_.service_actor(target);
        target_actor != nullptr) {
        caf::anon_send(target_actor, std::move(ingress));
    } else {
        SHIELD_LOG_WARNING(bridge_log(), "target service " + target +
                                             " has no actor for route " +
                                             std::to_string(route_id));
    }
}

void LuaGatewayBridge::on_disconnect(
    std::shared_ptr<shield::net::Session> session, std::string reason) {
    if (!session) return;

    if (registry_) {
        registry_->remove(session->id());
    }

    // Notify the current target service that its client went away. An empty
    // binding means the gateway actor already delivered Unbound on a kick
    // (it invalidates the binding before closing the socket); sending a
    // second notification would double-count the detach.
    const net::SessionBinding binding = session->binding();
    if (binding.target_service.empty()) {
        return;
    }
    if (caf::actor target = manager_.service_actor(binding.target_service);
        target != nullptr) {
        ClientControlMessage disconnected;
        disconnected.kind = ClientControlMessage::Kind::Disconnected;
        disconnected.context =
            client_context(auth_service_name_, session->id(), binding);
        disconnected.reason = std::move(reason);
        caf::anon_send(target, std::move(disconnected));
    }
}

}  // namespace shield::lua
