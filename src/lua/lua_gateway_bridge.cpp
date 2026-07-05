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
                                   std::string gateway_service_name)
    : manager_(manager),
      gateway_service_name_(std::move(gateway_service_name)) {}

void LuaGatewayBridge::on_connect(
    std::shared_ptr<shield::net::Session> session) {
    if (!session) return;

    const auto session_info = make_session_handle_json(session);

    auto* manager = &manager_;
    const auto target = gateway_service_name_;
    manager_.enqueue_forked_task(target, [manager, target, session_info]() {
        std::string error;
        if (!manager->send_system(target, "on_connect",
                                  nlohmann::json::array({session_info}),
                                  &error)) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_WARNING(log,
                               "Failed to queue gateway on_connect: " + error);
        }
    });
}

void LuaGatewayBridge::on_message(std::shared_ptr<shield::net::Session> session,
                                  const std::string& payload) {
    if (!session) return;

    const auto session_info = make_session_handle_json(session);

    auto* manager = &manager_;
    const auto target = gateway_service_name_;
    manager_.enqueue_forked_task(
        target, [manager, target, session_info, payload]() {
            std::string error;
            if (!manager->send_system(
                    target, "on_client_message",
                    nlohmann::json::array({session_info, payload}), &error)) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(
                    log, "Failed to queue gateway on_client_message: " + error);
            }
        });
}

void LuaGatewayBridge::on_packet(
    std::shared_ptr<shield::net::Session> session,
    const shield::transport::DispatchResult& packet) {
    if (!session) return;
    if (!packet.ok() || packet.should_drop() || packet.should_forward_raw()) {
        return;
    }
    if (!packet.decoded()) {
        return;
    }

    const auto session_info = make_session_handle_json(session);
    nlohmann::json payload;
    if (packet.decoded_body->has_message()) {
        payload = *packet.decoded_body->message;
    } else {
        payload = std::string(packet.decoded_body->bytes.begin(),
                              packet.decoded_body->bytes.end());
    }

    auto* manager = &manager_;
    const auto target = gateway_service_name_;
    manager_.enqueue_forked_task(target, [manager, target, session_info,
                                          payload = std::move(payload)]() {
        std::string error;
        if (!manager->send_system(target, "on_client_message",
                                  nlohmann::json::array({session_info, payload}),
                                  &error)) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_WARNING(
                log, "Failed to queue gateway on_client_message: " + error);
        }
    });
}

void LuaGatewayBridge::on_disconnect(
    std::shared_ptr<shield::net::Session> session, std::string reason) {
    if (!session) return;

    const auto session_info = make_session_handle_json(session);

    auto* manager = &manager_;
    const auto target = gateway_service_name_;
    manager_.enqueue_forked_task(
        target, [manager, target, session_info, reason = std::move(reason)]() {
            std::string error;
            if (!manager->send_system(
                    target, "on_disconnect",
                    nlohmann::json::array({session_info, reason}), &error)) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(
                    log, "Failed to queue gateway on_disconnect: " + error);
            }
        });
}

}  // namespace shield::lua
