#include "shield/net/session.hpp"

#include "shield/log/logger.hpp"

namespace shield::net {

std::atomic<uint64_t> Session::s_next_id(1);

Session::Session(boost::asio::ip::tcp::socket socket)
    : m_socket(std::move(socket)), m_id(s_next_id++) {}

void Session::start() {
    SHIELD_LOG_INFO << "Session " << m_id << " started for "
                    << m_socket.remote_endpoint();
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

void Session::send(const char *data, size_t length) {
    auto self(shared_from_this());
    auto buffer = std::vector<char>(data, data + length);

    boost::asio::post(m_socket.get_executor(),
                      [this, self, buf = std::move(buffer)]() mutable {
                          m_write_queue.push_back(std::move(buf));
                          if (!m_writing) {
                              do_write();
                          }
                      });
}

void Session::do_read() {
    auto self(shared_from_this());
    m_socket.async_read_some(
        boost::asio::buffer(m_read_buffer, max_length),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (m_read_callback) {
                    m_read_callback(m_read_buffer, length);
                }
                do_read();
            } else {
                if (ec != boost::asio::error::eof) {
                    SHIELD_LOG_ERROR << "Session " << m_id
                                     << " read error: " << ec.message();
                }
                close();
            }
        });
}

void Session::do_write() {
    if (m_write_queue.empty()) {
        m_writing = false;
        return;
    }

    m_writing = true;
    auto self(shared_from_this());
    auto &buf = m_write_queue.front();

    boost::asio::async_write(
        m_socket, boost::asio::buffer(buf.data(), buf.size()),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
            if (ec) {
                SHIELD_LOG_ERROR << "Session " << m_id
                                 << " write error: " << ec.message();
                m_writing = false;
                close();
                return;
            }
            m_write_queue.pop_front();
            do_write();
        });
}

}  // namespace shield::net
