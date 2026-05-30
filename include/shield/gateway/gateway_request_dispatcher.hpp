// [CORE]
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "caf/actor.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/gateway/middleware.hpp"
#include "shield/protocol/http_handler.hpp"
#include "shield/protocol/websocket_handler.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/service/service_context.hpp"

namespace shield::gateway {

class GatewayRequestDispatcher {
public:
    GatewayRequestDispatcher(actor::DistributedActorSystem& actor_system,
                             script::LuaVMPool& lua_vm_pool,
                             service::ServiceContext& svc_ctx,
                             std::chrono::milliseconds request_timeout =
                                 std::chrono::milliseconds{5000});

    // Route configuration
    void configure_http_routes(protocol::HttpRouter& router);

    // Per-protocol entry points (called by GatewayService)
    void handle_tcp_message(
        uint64_t connection_id, const std::string& message,
        std::function<void(const std::string&)> send_response);
    void handle_websocket_message(
        uint64_t connection_id, const std::string& message,
        protocol::WebSocketProtocolHandler& handler);

    void on_session_closed(uint64_t connection_id);

    // Middleware chain (applied to all protocols)
    MiddlewareChain& middleware_chain() { return middleware_chain_; }

    // Default Lua script paths per protocol
    std::string tcp_script = "scripts/player_actor.lua";
    std::string ws_script = "scripts/websocket_actor.lua";
    std::string http_script = "scripts/http_actor.lua";

private:
    // Unified dispatch: runs middleware chain then routes to actor.
    void dispatch(GatewayRequest& gw_req, GatewayResponse& gw_resp);

    caf::actor get_or_create_session_actor(uint64_t connection_id,
                                           const std::string& script_path);

    actor::DistributedActorSystem& actor_system_;
    script::LuaVMPool& lua_vm_pool_;
    service::ServiceContext& svc_ctx_;
    std::chrono::milliseconds request_timeout_;
    std::unordered_map<uint64_t, caf::actor> session_actors_;
    MiddlewareChain middleware_chain_;
};

}  // namespace shield::gateway
