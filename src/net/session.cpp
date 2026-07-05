// [SHIELD_NET] Session implementation
#include "shield/net/session.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <shared_mutex>
#include <unordered_map>

#include "shield/log/logger.hpp"
#include "shield/transport/frame.hpp"

namespace shield::net {

TcpSession::TcpSession(SessionId id, boost::asio::ip::tcp::socket socket,
                       SessionCallbacks callbacks, size_t max_frame_size)
    : id_(id),
      socket_(std::move(socket)),
      callbacks_(std::move(callbacks)),
      frame_decoder_(max_frame_size) {
    auto endpoint = socket_.remote_endpoint();
    remote_addr_.ip = endpoint.address().to_string();
    remote_addr_.port = endpoint.port();
    if (callbacks_.create_protocol_pipeline) {
        protocol_pipeline_ = callbacks_.create_protocol_pipeline();
    }
}

void TcpSession::start() {
    if (callbacks_.on_connect) {
        callbacks_.on_connect(shared_from_this());
    }
    do_receive();
}

bool TcpSession::send(const std::vector<uint8_t>& data) {
    if (!alive_.load()) return false;

    boost::system::error_code ec;
    boost::asio::write(socket_, boost::asio::buffer(data), ec);

    if (ec) {
        handle_error("send error: " + ec.message());
        return false;
    }

    return true;
}

bool TcpSession::send_message(const shield::transport::DecodedBody& message,
                              std::string* error) {
    if (!alive_.load()) {
        if (error) {
            *error = "session is closed";
        }
        return false;
    }
    if (!protocol_pipeline_) {
        if (error) {
            *error = "protocol pipeline is not configured";
        }
        return false;
    }

    auto encoded = protocol_pipeline_->encode_message(message);
    if (!protocol_pipeline_->error().empty()) {
        if (error) {
            *error = protocol_pipeline_->error();
        }
        return false;
    }
    if (encoded.empty()) {
        if (error) {
            *error = "protocol encode returned empty frame";
        }
        return false;
    }

    return send(encoded);
}

void TcpSession::close(std::string reason) {
    bool expected = true;
    if (!alive_.compare_exchange_strong(expected, false)) return;

    boost::system::error_code ec;
    socket_.close(ec);

    if (callbacks_.on_disconnect) {
        callbacks_.on_disconnect(shared_from_this(), reason);
    }
}

void TcpSession::do_receive() {
    if (!alive_.load()) return;

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

            if (protocol_pipeline_) {
                auto results = protocol_pipeline_->feed(receive_buffer_.data(),
                                                        receive_buffer_.size());
                if (!protocol_pipeline_->error().empty()) {
                    handle_error("protocol decode error: " +
                                 protocol_pipeline_->error());
                    return;
                }

                for (auto& result : results) {
                    if (!result.ok()) {
                        handle_error("protocol dispatch error: " +
                                     result.error);
                        return;
                    }
                    if (result.should_drop()) {
                        continue;
                    }
                    if (result.action ==
                            shield::transport::RouteAction::DecodeLocal &&
                        !protocol_pipeline_->materialize_decode(result)) {
                        handle_error("protocol decode error: " + result.error);
                        return;
                    }
                    if (callbacks_.on_packet) {
                        callbacks_.on_packet(shared_from_this(), result);
                    }
                }
            } else {
                // Process through legacy frame decoder.
                auto frames = frame_decoder_.feed(receive_buffer_.data(),
                                                  receive_buffer_.size());
                if (!frame_decoder_.error().empty()) {
                    handle_error("frame decode error: " +
                                 frame_decoder_.error());
                    return;
                }

                for (const auto& frame : frames) {
                    if (callbacks_.on_message) {
                        callbacks_.on_message(shared_from_this(),
                                              frame.payload());
                    }
                }
            }

            // Continue receiving
            do_receive();
        });
}

void TcpSession::handle_error(std::string reason) {
    auto& log = shield::log::get_logger("net");
    SHIELD_LOG_ERROR(log,
                     "Session " + std::to_string(id_) + " error: " + reason);
    // Map error reason to stable error code.
    if (reason.find("decode") != std::string::npos ||
        reason.find("frame") != std::string::npos) {
        error_code_ = "decode_error";
    } else if (reason.find("timeout") != std::string::npos) {
        error_code_ = "handshake_timeout";
    } else {
        error_code_ = "session_closed";
    }
    close(reason);
}

}  // namespace shield::net
