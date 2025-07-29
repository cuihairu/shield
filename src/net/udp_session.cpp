#include "shield/net/udp_session.hpp"

#include <iostream>

#include "shield/core/logger.hpp"

namespace shield::net {

std::atomic<uint64_t> UdpSession::s_next_session_id{1};

UdpSession::UdpSession(boost::asio::io_context& io_context, uint16_t port)
    : m_io_context(io_context),
      m_socket(io_context, boost::asio::ip::udp::endpoint(
                               boost::asio::ip::udp::v4(), port)),
      m_cleanup_timer(io_context) {
    SHIELD_LOG_INFO << "UdpSession created on port " << port;
}

UdpSession::~UdpSession() { stop(); }

void UdpSession::start() {
    if (m_running) {
        return;
    }

    m_running = true;
    do_receive();
    schedule_cleanup();

    SHIELD_LOG_INFO << "UdpSession started on port " << local_port();
}

void UdpSession::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;

    boost::system::error_code ec;
    m_socket.close(ec);
    m_cleanup_timer.cancel();

    m_endpoint_to_session.clear();
    m_endpoint_sessions.clear();

    SHIELD_LOG_INFO << "UdpSession stopped";
}

void UdpSession::send_to(const boost::asio::ip::udp::endpoint& endpoint,
                         const char* data, size_t length) {
    if (!m_running) {
        SHIELD_LOG_WARN << "Attempt to send on stopped UdpSession";
        return;
    }

    m_socket.async_send_to(
        boost::asio::buffer(data, length), endpoint,
        [this, endpoint](boost::system::error_code ec, std::size_t bytes_sent) {
            if (ec) {
                SHIELD_LOG_ERROR << "UDP send failed to "
                                 << endpoint.address().to_string() << ": "
                                 << ec.message();
            } else {
                SHIELD_LOG_DEBUG << "UDP sent " << bytes_sent << " bytes to "
                                 << endpoint.address().to_string();
            }
        });
}

void UdpSession::send_to(uint64_t session_id, const char* data, size_t length) {
    auto it = m_endpoint_sessions.find(session_id);
    if (it != m_endpoint_sessions.end()) {
        send_to(it->second->endpoint, data, length);
        it->second->update_activity();
    } else {
        SHIELD_LOG_WARN << "Attempt to send to unknown session ID: "
                        << session_id;
    }
}

uint64_t UdpSession::get_or_create_session_id(
    const boost::asio::ip::udp::endpoint& endpoint) {
    std::string endpoint_key =
        endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    auto it = m_endpoint_to_session.find(endpoint_key);
    if (it != m_endpoint_to_session.end()) {
        // Update activity for existing session
        auto session_it = m_endpoint_sessions.find(it->second);
        if (session_it != m_endpoint_sessions.end()) {
            session_it->second->update_activity();
            return it->second;
        }
    }

    // Create new session
    uint64_t session_id = s_next_session_id++;
    auto udp_endpoint = std::make_shared<UdpEndpoint>(endpoint, session_id);

    m_endpoint_to_session[endpoint_key] = session_id;
    m_endpoint_sessions[session_id] = udp_endpoint;

    SHIELD_LOG_DEBUG << "Created new UDP session " << session_id << " for "
                     << endpoint.address().to_string();

    return session_id;
}

void UdpSession::remove_session(uint64_t session_id) {
    auto it = m_endpoint_sessions.find(session_id);
    if (it != m_endpoint_sessions.end()) {
        auto endpoint = it->second->endpoint;
        std::string endpoint_key = endpoint.address().to_string() + ":" +
                                   std::to_string(endpoint.port());
        m_endpoint_to_session.erase(endpoint_key);
        m_endpoint_sessions.erase(it);

        SHIELD_LOG_DEBUG << "Removed UDP session " << session_id;
    }
}

void UdpSession::cleanup_expired_sessions() {
    std::vector<uint64_t> expired_sessions;

    for (const auto& [session_id, udp_endpoint] : m_endpoint_sessions) {
        if (udp_endpoint->is_expired(m_session_timeout)) {
            expired_sessions.push_back(session_id);
        }
    }

    for (uint64_t session_id : expired_sessions) {
        if (m_timeout_callback) {
            m_timeout_callback(session_id);
        }
        remove_session(session_id);
    }

    if (!expired_sessions.empty()) {
        SHIELD_LOG_DEBUG << "Cleaned up " << expired_sessions.size()
                         << " expired UDP sessions";
    }
}

void UdpSession::do_receive() {
    if (!m_running) {
        return;
    }

    m_socket.async_receive_from(
        boost::asio::buffer(m_receive_buffer, max_length), m_sender_endpoint,
        [this](boost::system::error_code ec, std::size_t bytes_received) {
            if (!ec && bytes_received > 0) {
                uint64_t session_id =
                    get_or_create_session_id(m_sender_endpoint);

                if (m_receive_callback) {
                    m_receive_callback(session_id, m_receive_buffer,
                                       bytes_received, m_sender_endpoint);
                }

                SHIELD_LOG_DEBUG
                    << "UDP received " << bytes_received << " bytes from "
                    << m_sender_endpoint.address().to_string() << " (session "
                    << session_id << ")";
            } else if (ec && m_running) {
                SHIELD_LOG_ERROR << "UDP receive error: " << ec.message();
            }

            if (m_running) {
                do_receive();
            }
        });
}

void UdpSession::schedule_cleanup() {
    if (!m_running) {
        return;
    }

    m_cleanup_timer.expires_after(m_cleanup_interval);
    m_cleanup_timer.async_wait([this](boost::system::error_code ec) {
        if (!ec && m_running) {
            cleanup_expired_sessions();
            schedule_cleanup();
        }
    });
}

}  // namespace shield::net