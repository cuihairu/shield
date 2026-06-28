// [SHIELD_LUA] Gateway bridge implementation
#include "shield/lua/lua_gateway_bridge.hpp"

#include <nlohmann/json.hpp>

#include "shield/log/logger.hpp"
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

    // Build session info as a JSON object that the Lua handler can read.
    // The Lua gateway_service.lua expects a table with id and remote_addr
    // fields.
    nlohmann::json session_info = {
        {"id", std::to_string(session->id())},
        {"remote_addr", session->remote_addr().to_string()}};

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

    nlohmann::json session_info = {
        {"id", std::to_string(session->id())},
        {"remote_addr", session->remote_addr().to_string()}};

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

    nlohmann::json session_info = {
        {"id", std::to_string(session->id())},
        {"remote_addr", session->remote_addr().to_string()}};

    nlohmann::json packet_info = {
        {"route_id", packet.packet.route_id},
        {"kind", packet.packet.kind},
        {"flags", packet.packet.flags},
        {"seq", packet.packet.seq},
        {"action", static_cast<int>(packet.action)},
        {"target_service",
         packet.route ? packet.route->target_service : std::uint32_t{0}},
        {"codec_id", packet.route ? packet.route->codec_id : std::uint16_t{0}},
        {"schema_id",
         packet.route ? packet.route->schema_id : std::uint16_t{0}},
        {"route", packet.route ? packet.route->debug_name : std::string{}}};

    const std::string payload(packet.packet.body.begin(),
                              packet.packet.body.end());

    auto* manager = &manager_;
    const auto target = gateway_service_name_;
    manager_.enqueue_forked_task(
        target, [manager, target, session_info, packet_info, payload]() {
            std::string error;
            if (!manager->send_system(
                    target, "on_client_packet",
                    nlohmann::json::array({session_info, packet_info, payload}),
                    &error)) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(
                    log, "Failed to queue gateway on_client_packet: " + error);
            }
        });
}

void LuaGatewayBridge::on_disconnect(
    std::shared_ptr<shield::net::Session> session, std::string reason) {
    if (!session) return;

    nlohmann::json session_info = {
        {"id", std::to_string(session->id())},
        {"remote_addr", session->remote_addr().to_string()}};

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
