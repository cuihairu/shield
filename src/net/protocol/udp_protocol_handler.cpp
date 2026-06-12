#include "shield/protocol/udp_protocol_handler.hpp"

#include "shield/log/logger.hpp"

namespace shield::protocol {

UdpProtocolHandler::UdpProtocolHandler(boost::asio::io_context& io_context,
                                       uint16_t port)
    : m_udp_session(
          std::make_unique<shield::net::UdpSession>(io_context, port)) {
    // Set up UDP session callbacks
    m_udp_session->on_receive(
        [this](uint64_t session_id, const char* data, size_t length,
               const boost::asio::ip::udp::endpoint& from) {
            on_udp_receive(session_id, data, length, from);
        });

    m_udp_session->on_timeout(
        [this](uint64_t session_id) { on_udp_timeout(session_id); });

    SHIELD_LOG_INFO << "UdpProtocolHandler created on port " << port;
}

UdpProtocolHandler::~UdpProtocolHandler() { stop(); }

void UdpProtocolHandler::start() {
    m_udp_session->start();
    SHIELD_LOG_INFO << "UdpProtocolHandler started";
}

void UdpProtocolHandler::stop() {
    if (m_udp_session) {
        m_udp_session->stop();
    }
    m_active_sessions.clear();
    SHIELD_LOG_INFO << "UdpProtocolHandler stopped";
}

void UdpProtocolHandler::handle_data(uint64_t connection_id, const char* data,
                                     size_t length) {
    // This method is primarily for compatibility with the IProtocolHandler
    // interface In UDP context, data handling is done through the UDP-specific
    // callback
    SHIELD_LOG_DEBUG << "handle_data called for UDP session " << connection_id;
}

void UdpProtocolHandler::handle_connection(uint64_t connection_id) {
    m_active_sessions.insert(connection_id);
    SHIELD_LOG_DEBUG << "UDP session " << connection_id << " connected";
}

void UdpProtocolHandler::handle_disconnection(uint64_t connection_id) {
    m_active_sessions.erase(connection_id);
    if (m_udp_session) {
        m_udp_session->remove_session(connection_id);
    }
    SHIELD_LOG_DEBUG << "UDP session " << connection_id << " disconnected";
}

bool UdpProtocolHandler::send_data(uint64_t connection_id,
                                   const std::string& data) {
    if (!m_udp_session) {
        SHIELD_LOG_ERROR << "Cannot send data: UdpSession is null";
        return false;
    }

    m_udp_session->send_to(connection_id, data.c_str(), data.size());
    return true;
}

void UdpProtocolHandler::send_to_endpoint(
    const boost::asio::ip::udp::endpoint& endpoint, const std::string& data) {
    if (!m_udp_session) {
        SHIELD_LOG_ERROR << "Cannot send data: UdpSession is null";
        return;
    }

    m_udp_session->send_to(endpoint, data.c_str(), data.size());
}

void UdpProtocolHandler::set_session_timeout(std::chrono::seconds timeout) {
    if (m_udp_session) {
        m_udp_session->set_session_timeout(timeout);
    }
}

void UdpProtocolHandler::set_cleanup_interval(std::chrono::seconds interval) {
    if (m_udp_session) {
        m_udp_session->set_cleanup_interval(interval);
    }
}

size_t UdpProtocolHandler::active_sessions() const {
    return m_udp_session ? m_udp_session->active_sessions() : 0;
}

uint16_t UdpProtocolHandler::local_port() const {
    return m_udp_session ? m_udp_session->local_port() : 0;
}

void UdpProtocolHandler::on_udp_receive(
    uint64_t session_id, const char* data, size_t length,
    const boost::asio::ip::udp::endpoint& from) {
    // Ensure session is tracked
    if (m_active_sessions.find(session_id) == m_active_sessions.end()) {
        handle_connection(session_id);
    }

    // Create UDP message and call the callback
    if (m_message_callback) {
        UdpMessage message(session_id, data, length, from);
        m_message_callback(message);
    }

    // Also call the generic handle_data for compatibility
    handle_data(session_id, data, length);
}

void UdpProtocolHandler::on_udp_timeout(uint64_t session_id) {
    if (m_session_timeout_callback) {
        m_session_timeout_callback(session_id);
    }

    // Clean up the session
    handle_disconnection(session_id);
}

}  // namespace shield::protocol