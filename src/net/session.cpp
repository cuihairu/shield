// [SHIELD_NET] Session implementation
#include "shield/net/session.hpp"

#include "shield/log/logger.hpp"
#include "shield/transport/frame.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <shared_mutex>
#include <unordered_map>

namespace shield::net {

TcpSession::TcpSession(SessionId id,
                       boost::asio::ip::tcp::socket socket,
                       SessionCallbacks callbacks)
    : id_(id),
      socket_(std::move(socket)),
      callbacks_(std::move(callbacks)) {

    auto endpoint = socket_.remote_endpoint();
    remote_addr_.ip = endpoint.address().to_string();
    remote_addr_.port = endpoint.port();
}

void TcpSession::start() {
    if (callbacks_.on_connect) {
        callbacks_.on_connect(shared_from_this());
    }
    do_receive();
}

bool TcpSession::send(const std::vector<uint8_t>& data) {
    if (!alive_) return false;

    boost::system::error_code ec;
    boost::asio::write(socket_, boost::asio::buffer(data), ec);

    if (ec) {
        handle_error("send error: " + ec.message());
        return false;
    }

    return true;
}

void TcpSession::close(std::string reason) {
    if (!alive_) return;

    alive_ = false;
    boost::system::error_code ec;
    socket_.close(ec);

    if (callbacks_.on_disconnect) {
        callbacks_.on_disconnect(shared_from_this(), reason);
    }
}

void TcpSession::do_receive() {
    if (!alive_) return;

    // Read into buffer (resize to max frame size)
    receive_buffer_.resize(64 * 1024);

    socket_.async_read_some(
        boost::asio::buffer(receive_buffer_),
        [this, self = shared_from_this()](auto ec, auto bytes_read) {
            if (ec) {
                handle_error("receive error: " + ec.message());
                return;
            }

            receive_buffer_.resize(bytes_read);

            // Process through frame decoder
            auto frames = frame_decoder_.feed(receive_buffer_.data(),
                                             receive_buffer_.size());

            for (const auto& frame : frames) {
                if (callbacks_.on_message) {
                    callbacks_.on_message(shared_from_this(), frame.payload());
                }
            }

            // Continue receiving
            do_receive();
        }
    );
}

void TcpSession::handle_error(std::string reason) {
    auto& log = shield::log::get_logger("net");
    SHIELD_LOG_ERROR(log, "Session " + std::to_string(id_) + " error: " + reason);
    close(reason);
}

}  // namespace shield::net
