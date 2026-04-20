#include "shield/gateway/gateway_service.hpp"

#include "shield/caf_type_ids.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/logger.hpp"
#include "shield/protocol/binary_protocol.hpp"

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
    m_request_dispatcher = std::make_unique<GatewayRequestDispatcher>(
        m_actor_system, m_lua_vm_pool, m_request_timeout);

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
    if (m_config->http.enabled && m_config->http.backend == "legacy") {
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
                m_request_dispatcher->on_session_closed(session->id());
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
                m_request_dispatcher->on_session_closed(session->id());
            });

            return session;
        });
    }

    setup_protocol_handlers();

    // Beast HTTP server configuration
    if (m_config->http.enabled && m_config->http.backend == "beast") {
        http::BeastHttpServerConfig http_cfg;
        http_cfg.host = listener_config.host;
        http_cfg.port = m_config->http.port;
        http_cfg.threads = io_threads;
        http_cfg.root_path = m_config->http.root_path;
        http_cfg.max_request_size =
            static_cast<std::size_t>(m_config->http.max_request_size);

        m_beast_http_server = std::make_unique<http::BeastHttpServer>(
            std::move(http_cfg), [this](const protocol::HttpRequest& req) {
                return m_http_handler->get_router().route_request(req);
            });
    }
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
    if (m_beast_http_server) {
        m_beast_http_server->start();
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
    if (m_beast_http_server) {
        m_beast_http_server->stop();
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

    m_request_dispatcher->configure_http_routes(m_http_handler->get_router());

    // Create WebSocket handler
    m_websocket_handler = protocol::create_websocket_handler();
    m_websocket_handler->set_session_provider(
        [this](uint64_t session_id) { return get_session(session_id); });

    // Set WebSocket message handler
    m_websocket_handler->set_message_handler(
        [this](uint64_t connection_id, const std::string& message) {
            m_request_dispatcher->handle_websocket_message(
                connection_id, message, *m_websocket_handler);
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
                auto session_id = session->id();
                m_request_dispatcher->handle_tcp_message(
                    session_id, message_str,
                    [this, session_id](const std::string& response) {
                        send_binary_json_response(session_id, response);
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
        m_request_dispatcher->on_session_closed(session->id());
        m_sessions.erase(session->id());
        m_session_protocols.erase(session->id());
    });
}

void GatewayService::send_binary_json_response(uint64_t session_id,
                                               const std::string& response) {
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
