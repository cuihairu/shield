#include "shield/net/session.hpp"
#include "shield/core/logger.hpp"

namespace shield::net {

std::atomic<uint64_t> Session::s_next_id(1);

Session::Session(boost::asio::ip::tcp::socket socket)
    : m_socket(std::move(socket)),
      m_id(s_next_id++) {}

void Session::start() {
    SHIELD_LOG_INFO << "Session " << m_id << " started for " << m_socket.remote_endpoint();
    do_read();
}

void Session::close() {
    boost::system::error_code ec;
    m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    m_socket.close(ec);
    if (m_close_callback) {
        m_close_callback();
    }
}

void Session::send(const char* data, size_t length) {
    auto self(shared_from_this());
    boost::asio::async_write(m_socket, boost::asio::buffer(data, length),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
            if (ec) {
                SHIELD_LOG_ERROR << "Session " << m_id << " write error: " << ec.message();
                close();
            }
        });
}

void Session::do_read() {
    auto self(shared_from_this());
    m_socket.async_read_some(boost::asio::buffer(m_read_buffer, max_length),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (m_read_callback) {
                    m_read_callback(m_read_buffer, length);
                }
                do_read();
            } else {
                if (ec != boost::asio::error::eof) {
                    SHIELD_LOG_ERROR << "Session " << m_id << " read error: " << ec.message();
                }
                close();
            }
        });
}

} // namespace shield::net
