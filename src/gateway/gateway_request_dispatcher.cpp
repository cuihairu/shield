#include "shield/gateway/gateway_request_dispatcher.hpp"

#include <nlohmann/json.hpp>

#include "shield/actor/lua_actor.hpp"
#include "shield/log/logger.hpp"

namespace shield::gateway {

GatewayRequestDispatcher::GatewayRequestDispatcher(
    actor::DistributedActorSystem& actor_system,
    script::LuaVMPool& lua_vm_pool,
    service::ServiceContext& svc_ctx,
    std::chrono::milliseconds request_timeout)
    : actor_system_(actor_system),
      lua_vm_pool_(lua_vm_pool),
      svc_ctx_(svc_ctx),
      request_timeout_(request_timeout) {}

void GatewayRequestDispatcher::dispatch(GatewayRequest& gw_req,
                                        GatewayResponse& gw_resp) {
    // Resolve the script path based on protocol
    std::string script;
    switch (gw_req.protocol) {
        case protocol::ProtocolType::HTTP:
            script = http_script;
            break;
        case protocol::ProtocolType::WEBSOCKET:
            script = ws_script;
            break;
        default:
            script = tcp_script;
            break;
    }

    try {
        auto lua_actor_ref =
            get_or_create_session_actor(gw_req.session_id, script);

        // Use CAF request-response with a temporary actor
        // Synchronous wait via scoped_actor for simplicity in dispatch
        caf::scoped_actor scoped(actor_system_.system());
        std::string msg_type = gw_req.method + ":" + gw_req.path;
        auto result = scoped->request(
            lua_actor_ref, request_timeout_, msg_type, gw_req.body);
        result.receive(
            [&](const std::string& response) {
                gw_resp.body = response;
                gw_resp.success = true;
                gw_resp.status_code = 200;
            },
            [&](caf::error& err) {
                SHIELD_LOG_ERROR << "dispatch request failed for session "
                                 << gw_req.session_id << ": "
                                 << caf::to_string(err);
                gw_resp.success = false;
                gw_resp.status_code = 504;
                gw_resp.body =
                    R"({"success":false,"error_message":"Request timeout or actor error"})";
            });
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "dispatch error: " << e.what();
        gw_resp.success = false;
        gw_resp.status_code = 500;
        gw_resp.body =
            R"({"success":false,"error_message":"Internal dispatch error"})";
    }
}

void GatewayRequestDispatcher::configure_http_routes(
    protocol::HttpRouter& router) {
    router.add_route(
        "POST", "/api/game/action",
        [this](const protocol::HttpRequest& request) {
            protocol::HttpResponse response;

            GatewayRequest gw_req;
            gw_req.session_id = request.connection_id;
            gw_req.path = request.path;
            gw_req.method = request.method;
            gw_req.body = request.body;
            gw_req.headers = request.headers;
            gw_req.protocol = protocol::ProtocolType::HTTP;

            GatewayResponse gw_resp;
            middleware_chain_.execute(
                gw_req, gw_resp,
                [this](GatewayRequest& r, GatewayResponse& s) {
                    dispatch(r, s);
                });

            response.status_code = gw_resp.status_code;
            response.body = gw_resp.body;
            for (auto& [k, v] : gw_resp.headers) {
                response.headers[k] = v;
            }
            return response;
        });

    router.add_route(
        "GET", "/api/player/info", [](const protocol::HttpRequest&) {
            protocol::HttpResponse response;
            response.body =
                R"({"player_id": "test_player", "level": 10, "score": 1000})";
            return response;
        });
}

void GatewayRequestDispatcher::handle_tcp_message(
    uint64_t connection_id, const std::string& message,
    std::function<void(const std::string&)> send_response) {
    GatewayRequest gw_req;
    gw_req.session_id = connection_id;
    gw_req.path = "/tcp";
    gw_req.method = "TCP";
    gw_req.body = message;
    gw_req.protocol = protocol::ProtocolType::TCP;

    GatewayResponse gw_resp;
    middleware_chain_.execute(
        gw_req, gw_resp,
        [this](GatewayRequest& r, GatewayResponse& s) { dispatch(r, s); });

    send_response(gw_resp.body);
}

void GatewayRequestDispatcher::handle_websocket_message(
    uint64_t connection_id, const std::string& message,
    protocol::WebSocketProtocolHandler& handler) {
    GatewayRequest gw_req;
    gw_req.session_id = connection_id;
    gw_req.path = "/ws";
    gw_req.method = "WS";

    // Try to extract "type" from the message for routing
    try {
        auto j = nlohmann::json::parse(message);
        if (j.contains("type") && j["type"].is_string()) {
            gw_req.path = std::string("/ws/") + j["type"].get<std::string>();
        }
    } catch (const nlohmann::json::parse_error&) {
        // Non-JSON message, use default WebSocket path
    }

    gw_req.body = message;
    gw_req.protocol = protocol::ProtocolType::WEBSOCKET;

    GatewayResponse gw_resp;
    middleware_chain_.execute(
        gw_req, gw_resp,
        [this](GatewayRequest& r, GatewayResponse& s) { dispatch(r, s); });

    handler.send_text_frame(connection_id, gw_resp.body);
}

void GatewayRequestDispatcher::on_session_closed(uint64_t connection_id) {
    session_actors_.erase(connection_id);
}

caf::actor GatewayRequestDispatcher::get_or_create_session_actor(
    uint64_t connection_id, const std::string& script_path) {
    auto actor_it = session_actors_.find(connection_id);
    if (actor_it != session_actors_.end()) {
        return actor_it->second;
    }

    auto lua_actor = actor::create_lua_actor(
        actor_system_.system(), lua_vm_pool_, actor_system_, svc_ctx_,
        script_path, std::to_string(connection_id));
    session_actors_[connection_id] = lua_actor;
    return lua_actor;
}

}  // namespace shield::gateway
