#include "shield/gateway/game_gateway.hpp"

#include "shield/gateway/gateway_request_dispatcher.hpp"
#include "shield/log/logger.hpp"

namespace shield::gateway {

void GameGateway::setup_login_route(protocol::HttpRouter& router,
                                     const std::string& lua_script) {
    router.add_route(
        "POST", "/login",
        [&router, lua_script](const protocol::HttpRequest& request) {
            protocol::HttpResponse response;
            response.headers["Content-Type"] = "application/json";

            // The actual login logic lives in the Lua script.
            // Here we provide the HTTP → actor bridge.
            // In production, GatewayRequestDispatcher.dispatch() handles this.
            // This route serves as a template / entry point.
            response.body =
                R"({"success":true,"message":"Login route active","script":")" +
                lua_script + R"("})";

            SHIELD_LOG_INFO << "Login route registered with script: "
                            << lua_script;
            return response;
        });

    SHIELD_LOG_INFO << "GameGateway: login route registered";
}

void GameGateway::setup_session_handler(
    protocol::WebSocketProtocolHandler& ws_handler,
    const std::string& lua_script) {
    ws_handler.set_message_handler(
        [&ws_handler, lua_script](uint64_t connection_id,
                                   const std::string& message) {
            SHIELD_LOG_INFO << "GameGateway WS session " << connection_id
                            << " message: " << message.substr(0, 100);

            // Echo back for template purposes. In production,
            // GatewayRequestDispatcher routes to Lua actor.
            ws_handler.send_text_frame(
                connection_id,
                R"({"success":true,"session_id":")" +
                    std::to_string(connection_id) + R"("})");
        });

    SHIELD_LOG_INFO << "GameGateway: session handler registered with script: "
                    << lua_script;
}

void GameGateway::setup_message_dispatch(GatewayRequestDispatcher& dispatcher,
                                          const std::string& default_script) {
    dispatcher.tcp_script = default_script;
    dispatcher.ws_script = default_script;
    SHIELD_LOG_INFO
        << "GameGateway: message dispatch configured with default script: "
        << default_script;
}

}  // namespace shield::gateway
