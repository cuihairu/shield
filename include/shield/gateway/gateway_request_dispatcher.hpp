#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "caf/actor.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/protocol/http_handler.hpp"
#include "shield/protocol/websocket_handler.hpp"
#include "shield/script/lua_vm_pool.hpp"

namespace shield::gateway {

class GatewayRequestDispatcher {
public:
    GatewayRequestDispatcher(actor::DistributedActorSystem& actor_system,
                             script::LuaVMPool& lua_vm_pool,
                             std::chrono::milliseconds request_timeout =
                                 std::chrono::milliseconds{5000});

    void configure_http_routes(protocol::HttpRouter& router);
    void handle_tcp_message(
        uint64_t connection_id, const std::string& message,
        std::function<void(const std::string&)> send_response);
    void handle_websocket_message(uint64_t connection_id,
                                  const std::string& message,
                                  protocol::WebSocketProtocolHandler& handler);
    void on_session_closed(uint64_t connection_id);

private:
    caf::actor get_or_create_session_actor(uint64_t connection_id,
                                           const std::string& script_path);

    actor::DistributedActorSystem& actor_system_;
    script::LuaVMPool& lua_vm_pool_;
    std::chrono::milliseconds request_timeout_;
    std::unordered_map<uint64_t, caf::actor> session_actors_;
};

}  // namespace shield::gateway
