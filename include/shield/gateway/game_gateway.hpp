// [CORE]
#pragma once

#include <string>

#include "shield/protocol/http_handler.hpp"
#include "shield/protocol/websocket_handler.hpp"

namespace shield::gateway {

class GatewayRequestDispatcher;

// Pre-built gateway patterns for common game server scenarios.
// Call these from GatewayService or application startup to wire up
// login, session, and message dispatch routes.
class GameGateway {
public:
    // HTTP POST /login → validate credentials → create session → return token
    static void setup_login_route(protocol::HttpRouter& router,
                                   const std::string& lua_script);

    // WebSocket on-connect: authenticate, bind to a player actor, set up heartbeat.
    static void setup_session_handler(
        protocol::WebSocketProtocolHandler& ws_handler,
        const std::string& lua_script);

    // TCP/WS message dispatch: route {"type":"xxx"} to the correct Lua actor.
    static void setup_message_dispatch(GatewayRequestDispatcher& dispatcher,
                                        const std::string& default_script);
};

}  // namespace shield::gateway
