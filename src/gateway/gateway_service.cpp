#include "shield/gateway/gateway_service.hpp"

#include "shield/actor/lua_actor.hpp"
#include "shield/caf_type_ids.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/logger.hpp"
#include "shield/serialization/json_universal_serializer.hpp"

namespace shield::gateway {

GatewayService::GatewayService(const std::string& name,
                               actor::DistributedActorSystem& actor_system,
                               script::LuaVMPool& lua_vm_pool,
                               std::shared_ptr<GatewayConfig> config)
    : m_actor_system(actor_system),
      m_lua_vm_pool(lua_vm_pool),
      m_config(config),
      m_name(name) {}

GatewayService::~GatewayService() = default;

void GatewayService::on_init(core::ApplicationContext& ctx) {
    SHIELD_LOG_INFO << "Initializing GatewayService: " << name();

    if (!m_config) {
        throw std::runtime_error("GatewayConfig is null");
    }

    // Validate configuration
    m_config->validate();

    // TCP server configuration
    const auto& listener_config = m_config->listener;
    int io_threads = m_config->get_effective_io_threads();

    m_master_reactor = std::make_unique<net::MasterReactor>(
        listener_config.host, listener_config.port, io_threads);
    m_master_reactor->set_session_creator([this](auto socket) {
        auto session = std::make_shared<net::Session>(std::move(socket));
        setup_session(session);
        return session;
    });

    // HTTP server configuration
    if (m_config->http.enabled) {
        m_http_reactor = std::make_unique<net::MasterReactor>(
            listener_config.host, m_config->http.port, io_threads);

        m_http_reactor->set_session_creator([this](auto socket) {
            auto session = std::make_shared<net::Session>(std::move(socket));
            // HTTP session special handling
            m_sessions[session->id()] = std::weak_ptr<net::Session>(session);
            m_session_protocols[session->id()] = protocol::ProtocolType::HTTP;

            m_http_handler->handle_connection(session->id());

            session->on_read([this, session](const char* data, size_t length) {
                m_http_handler->handle_data(session->id(), data, length);
            });

            session->on_close([this, session]() {
                m_http_handler->handle_disconnection(session->id());
                m_sessions.erase(session->id());
                m_session_protocols.erase(session->id());
            });

            return session;
        });
    }

    // WebSocket server configuration
    if (m_config->websocket.enabled) {
        m_ws_reactor = std::make_unique<net::MasterReactor>(
            listener_config.host, m_config->websocket.port, io_threads);

        m_ws_reactor->set_session_creator([this](auto socket) {
            auto session = std::make_shared<net::Session>(std::move(socket));
            // WebSocket session special handling
            m_sessions[session->id()] = std::weak_ptr<net::Session>(session);
            m_session_protocols[session->id()] =
                protocol::ProtocolType::WEBSOCKET;

            m_websocket_handler->handle_connection(session->id());

            session->on_read([this, session](const char* data, size_t length) {
                m_websocket_handler->handle_data(session->id(), data, length);
            });

            session->on_close([this, session]() {
                m_websocket_handler->handle_disconnection(session->id());
                m_sessions.erase(session->id());
                m_session_protocols.erase(session->id());
                m_session_actors.erase(session->id());
            });

            return session;
        });
    }

    setup_protocol_handlers();
}

void GatewayService::on_start() {
    SHIELD_LOG_INFO << "Starting GatewayService: " << name();

    // Start TCP server
    m_master_reactor->start();

    // Start HTTP server
    if (m_http_reactor) {
        m_http_reactor->start();
        SHIELD_LOG_INFO << "HTTP server started on port "
                        << m_config->http.port;
    }

    // Start WebSocket server
    if (m_ws_reactor) {
        m_ws_reactor->start();
        SHIELD_LOG_INFO << "WebSocket server started on port "
                        << m_config->websocket.port;
    }
}

void GatewayService::on_stop() {
    SHIELD_LOG_INFO << "Stopping GatewayService: " << name();

    if (m_master_reactor) {
        m_master_reactor->stop();
    }

    if (m_http_reactor) {
        m_http_reactor->stop();
    }

    if (m_ws_reactor) {
        m_ws_reactor->stop();
    }
}

void GatewayService::on_config_reloaded() {
    SHIELD_LOG_INFO << "GatewayService config reloaded";
    // Get the latest config
    m_config = config::ConfigManager::instance()
                   .get_configuration_properties<GatewayConfig>();
    // Stop the reactors to apply new settings
    on_stop();
    // Restart the reactors with new settings
    on_start();
}

void GatewayService::setup_protocol_handlers() {
    // Create HTTP handler
    m_http_handler = protocol::create_http_handler();
    m_http_handler->set_session_provider(
        [this](uint64_t session_id) { return get_session(session_id); });

    setup_http_routes();

    // Create WebSocket handler
    m_websocket_handler = protocol::create_websocket_handler();
    m_websocket_handler->set_session_provider(
        [this](uint64_t session_id) { return get_session(session_id); });

    // Set WebSocket message handler
    m_websocket_handler->set_message_handler([this](
                                                 uint64_t connection_id,
                                                 const std::string& message) {
        try {
            // Create or get LuaActor for this connection
            auto actor_it = m_session_actors.find(connection_id);
            if (actor_it == m_session_actors.end()) {
                auto lua_actor = actor::create_lua_actor(
                    m_actor_system.system(), m_lua_vm_pool, m_actor_system,
                    "scripts/websocket_actor.lua",
                    std::to_string(connection_id));
                m_session_actors[connection_id] = lua_actor;
                actor_it = m_session_actors.find(connection_id);
            }

            // Send message string to LuaActor
            auto lua_actor_ref = actor_it->second;
            auto temp_actor = m_actor_system.system().spawn(
                [this, connection_id, lua_actor_ref,
                 message](caf::event_based_actor* self) -> caf::behavior {
                    self->request(lua_actor_ref, m_request_timeout,
                                  std::string("websocket_message"), message)
                        .then(
                            [this, connection_id,
                             self](const std::string& response) {
                                // Send response back to WebSocket client
                                m_websocket_handler->send_text_frame(
                                    connection_id, response);
                                self->quit();
                            },
                            [=, this](caf::error& err) {
                                SHIELD_LOG_ERROR
                                    << "WebSocket LuaActor request failed for "
                                       "connection "
                                    << connection_id << ": "
                                    << caf::to_string(err);

                                std::string error_response =
                                    R"({"success": false, "error_message": "Request timeout or actor error"})";
                                m_websocket_handler->send_text_frame(
                                    connection_id, error_response);
                                self->quit();
                            });

                    return caf::behavior{};
                });

        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "WebSocket message processing error: "
                             << e.what();
        }
    });
}

void GatewayService::setup_http_routes() {
    auto& router = m_http_handler->get_router();

    // Game API routes
    router.add_route(
        "POST", "/api/game/action",
        [this](const protocol::HttpRequest& request) {
            protocol::HttpResponse response;

            try {
                // Create temporary actor to handle request
                auto lua_actor = actor::create_lua_actor(
                    m_actor_system.system(), m_lua_vm_pool, m_actor_system,
                    "scripts/http_actor.lua",
                    std::to_string(request.connection_id));

                // Simplified handling, return success response
                response.body =
                    R"({"status": "accepted", "message": "Request queued for processing"})";

            } catch (const std::exception& e) {
                response.status_code = 400;
                response.status_text = "Bad Request";
                response.body = R"({"error": "Invalid request format"})";
            }

            return response;
        });

    // Player info API
    router.add_route(
        "GET", "/api/player/info", [](const protocol::HttpRequest& request) {
            protocol::HttpResponse response;
            response.body =
                R"({"player_id": "test_player", "level": 10, "score": 1000})";
            return response;
        });
}

std::shared_ptr<net::Session> GatewayService::get_session(uint64_t session_id) {
    auto it = m_sessions.find(session_id);
    if (it != m_sessions.end()) {
        return it->second.lock();
    }
    return nullptr;
}

protocol::ProtocolType GatewayService::detect_protocol(uint64_t connection_id,
                                                       const char* data,
                                                       size_t length) {
    if (length == 0) return protocol::ProtocolType::TCP;

    // Detect HTTP request
    if (length >= 3) {
        std::string prefix(data, 3);
        if (prefix == "GET" || prefix == "POST" || prefix == "PUT" ||
            prefix == "DEL") {
            return protocol::ProtocolType::HTTP;
        }
    }

    // Detect WebSocket handshake
    if (length >= 14) {
        std::string start(data, 14);
        if (start == "GET" &&
            std::string(data, length).find("Upgrade: websocket") !=
                std::string::npos) {
            return protocol::ProtocolType::WEBSOCKET;
        }
    }

    return protocol::ProtocolType::TCP;
}

void GatewayService::setup_session(std::shared_ptr<net::Session> session) {
    // Store a receive buffer for this session
    m_session_recv_buffers[session->id()] = std::vector<char>();

    // Store weak reference to session for later access
    m_sessions[session->id()] = std::weak_ptr<net::Session>(session);

    // Spawn a LuaActor for this session
    auto lua_actor = actor::create_lua_actor(
        m_actor_system.system(), m_lua_vm_pool, m_actor_system,
        "scripts/player_actor.lua", std::to_string(session->id()));
    m_session_actors[session->id()] = lua_actor;

    session->on_read([this, session](const char* data, size_t length) {
        // Append received data to the session's buffer
        auto& recv_buffer = m_session_recv_buffers[session->id()];
        recv_buffer.insert(recv_buffer.end(), data, data + length);

        // Process messages from the buffer
        size_t bytes_processed = 0;
        while (true) {
            auto [message_str, consumed] = protocol::BinaryProtocol::decode(
                recv_buffer.data() + bytes_processed,
                recv_buffer.size() - bytes_processed);
            if (consumed > 0) {
                // Send message to the associated LuaActor using
                // request-response pattern
                auto session_id = session->id();
                auto lua_actor_ref = m_session_actors[session_id];

                // Create a temporary actor to handle the request-response
                // communication
                auto temp_actor = m_actor_system.system().spawn(
                    [this, session_id, lua_actor_ref, message_str](
                        caf::event_based_actor* self) -> caf::behavior {
                        // Send request to LuaActor and handle response
                        self->request(lua_actor_ref, m_request_timeout,
                                      std::string("tcp_message"), message_str)
                            .then(
                                [this, session_id,
                                 self](const std::string& response) {
                                    // Handle the response from LuaActor
                                    handle_lua_actor_response_json(session_id,
                                                                   response);
                                    self->quit();
                                },
                                [=, this](caf::error& err) {
                                    // Handle timeout or error
                                    SHIELD_LOG_ERROR << "Request to LuaActor "
                                                        "failed for session "
                                                     << session_id << ": "
                                                     << caf::to_string(err);

                                    // Send error response back to client
                                    std::string error_response =
                                        R"({"success": false, "error_message": "Request timeout or actor error"})";
                                    handle_lua_actor_response_json(
                                        session_id, error_response);
                                    self->quit();
                                });

                        // Return empty behavior as the actor will quit after
                        // handling the request
                        return caf::behavior{};
                    });

                bytes_processed += consumed;
            } else {
                // Not enough data for a full message, or invalid message
                break;
            }
        }
        // Remove processed bytes from the buffer
        if (bytes_processed > 0) {
            recv_buffer.erase(recv_buffer.begin(),
                              recv_buffer.begin() + bytes_processed);
        }
    });

    session->on_close([this, session]() {
        SHIELD_LOG_INFO << "Session " << session->id() << " closed.";
        // Remove the session's receive buffer and actor when the session closes
        m_session_recv_buffers.erase(session->id());
        m_session_actors.erase(session->id());
        m_sessions.erase(session->id());
        m_session_protocols.erase(session->id());
    });
}

void GatewayService::handle_lua_actor_response(
    uint64_t session_id, const actor::LuaResponse& response) {
    // Find the session
    auto session_it = m_sessions.find(session_id);
    if (session_it == m_sessions.end()) {
        SHIELD_LOG_WARN << "Cannot find session " << session_id
                        << " to send response";
        return;
    }

    // Check if session is still alive
    auto session_ptr = session_it->second.lock();
    if (!session_ptr) {
        SHIELD_LOG_WARN << "Session " << session_id
                        << " is no longer alive, discarding response";
        m_sessions.erase(session_it);  // Clean up dead weak_ptr
        return;
    }

    try {
        // Serialize LuaResponse to JSON using convenience function
        std::string json_response =
            shield::serialization::to_json_string(response);

        // Encode the JSON response using binary protocol
        auto encoded_response = protocol::BinaryProtocol::encode(json_response);

        // Send response back to client
        session_ptr->send(encoded_response.data(), encoded_response.size());

        SHIELD_LOG_DEBUG << "Sent response to session " << session_id
                         << ", success: " << response.success
                         << ", data size: " << response.data.size();
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to serialize/send response to session "
                         << session_id << ": " << e.what();
    }
}

void GatewayService::handle_lua_actor_response_json(
    uint64_t session_id, const std::string& response) {
    // Find the session
    auto session_it = m_sessions.find(session_id);
    if (session_it == m_sessions.end()) {
        SHIELD_LOG_WARN << "Cannot find session " << session_id
                        << " to send response";
        return;
    }

    // Check if session is still alive
    auto session_ptr = session_it->second.lock();
    if (!session_ptr) {
        SHIELD_LOG_WARN << "Session " << session_id
                        << " is no longer alive, discarding response";
        m_sessions.erase(session_it);  // Clean up dead weak_ptr
        return;
    }

    try {
        // Encode the JSON response using binary protocol
        auto encoded_response = protocol::BinaryProtocol::encode(response);

        // Send response back to client
        session_ptr->send(encoded_response.data(), encoded_response.size());

        SHIELD_LOG_DEBUG << "Sent JSON response to session " << session_id
                         << ", response: " << response;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to serialize/send response to session "
                         << session_id << ": " << e.what();
    }
}

}  // namespace shield::gateway
