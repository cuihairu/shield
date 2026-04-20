#include "shield/gateway/gateway_request_dispatcher.hpp"

#include "shield/actor/lua_actor.hpp"
#include "shield/log/logger.hpp"

namespace shield::gateway {

GatewayRequestDispatcher::GatewayRequestDispatcher(
    actor::DistributedActorSystem& actor_system,
    script::LuaVMPool& lua_vm_pool,
    std::chrono::milliseconds request_timeout)
    : actor_system_(actor_system),
      lua_vm_pool_(lua_vm_pool),
      request_timeout_(request_timeout) {}

void GatewayRequestDispatcher::configure_http_routes(protocol::HttpRouter& router) {
    router.add_route(
        "POST", "/api/game/action",
        [this](const protocol::HttpRequest& request) {
            protocol::HttpResponse response;

            try {
                auto lua_actor = get_or_create_session_actor(
                    request.connection_id, "scripts/http_actor.lua");
                (void)lua_actor;

                response.body =
                    R"({"status": "accepted", "message": "Request queued for processing"})";
            } catch (const std::exception&) {
                response.status_code = 400;
                response.status_text = "Bad Request";
                response.body = R"({"error": "Invalid request format"})";
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
    try {
        auto lua_actor_ref =
            get_or_create_session_actor(connection_id, "scripts/player_actor.lua");

        auto temp_actor = actor_system_.system().spawn(
            [this, connection_id, lua_actor_ref, message,
             send_response = std::move(send_response)](
                caf::event_based_actor* self) mutable -> caf::behavior {
                self->request(lua_actor_ref, request_timeout_,
                              std::string("tcp_message"), message)
                    .then(
                        [send_response = std::move(send_response),
                         self](const std::string& response) mutable {
                            send_response(response);
                            self->quit();
                        },
                        [connection_id,
                         send_response = std::move(send_response),
                         self](caf::error& err) mutable {
                            SHIELD_LOG_ERROR
                                << "TCP LuaActor request failed for connection "
                                << connection_id << ": "
                                << caf::to_string(err);

                            send_response(
                                R"({"success": false, "error_message": "Request timeout or actor error"})");
                            self->quit();
                        });

                return caf::behavior{};
            });

        (void)temp_actor;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "TCP message processing error: " << e.what();
        send_response(
            R"({"success": false, "error_message": "Message processing error"})");
    }
}

void GatewayRequestDispatcher::handle_websocket_message(
    uint64_t connection_id, const std::string& message,
    protocol::WebSocketProtocolHandler& handler) {
    try {
        auto lua_actor_ref = get_or_create_session_actor(
            connection_id, "scripts/websocket_actor.lua");

        auto temp_actor = actor_system_.system().spawn(
            [this, connection_id, lua_actor_ref, message,
             &handler](caf::event_based_actor* self) -> caf::behavior {
                self->request(lua_actor_ref, request_timeout_,
                              std::string("websocket_message"), message)
                    .then(
                        [&handler, connection_id,
                         self](const std::string& response) {
                            handler.send_text_frame(connection_id, response);
                            self->quit();
                        },
                        [&handler, connection_id, self](caf::error& err) {
                            SHIELD_LOG_ERROR
                                << "WebSocket LuaActor request failed for "
                                   "connection "
                                << connection_id << ": "
                                << caf::to_string(err);

                            std::string error_response =
                                R"({"success": false, "error_message": "Request timeout or actor error"})";
                            handler.send_text_frame(connection_id,
                                                    error_response);
                            self->quit();
                        });

                return caf::behavior{};
            });

        (void)temp_actor;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "WebSocket message processing error: "
                         << e.what();
    }
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
        actor_system_.system(), lua_vm_pool_, actor_system_, script_path,
        std::to_string(connection_id));
    session_actors_[connection_id] = lua_actor;
    return lua_actor;
}

}  // namespace shield::gateway
