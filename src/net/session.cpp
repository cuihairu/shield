// [SHIELD_NET] Session implementation
#include "shield/net/session.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <shared_mutex>
#include <unordered_map>

#include "shield/log/logger.hpp"
#include "shield/transport/frame.hpp"

namespace shield::net {

TcpSession::TcpSession(SessionId id, boost::asio::ip::tcp::socket socket,
                       SessionCallbacks callbacks, size_t max_frame_size,
                       size_t max_send_queue, uint32_t read_idle_timeout_ms)
    : id_(id),
      socket_(std::move(socket)),
      strand_(boost::asio::make_strand(socket_.get_executor())),
      callbacks_(std::move(callbacks)),
      frame_decoder_(max_frame_size),
      max_send_queue_(max_send_queue),
      read_deadline_(socket_.get_executor()),
      read_idle_timeout_ms_(read_idle_timeout_ms) {
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

bool TcpSession::send(const std::vector<uint8_t>& data, std::string* error) {
    if (!alive_.load()) {
        if (error) {
            *error = "session is closed";
        }
        return false;
    }
    if (data.empty()) {
        return true;
    }

    // Soft backpressure: reject once the queued message count is already at
    // the limit. An empty queue always accepts so the first message can still
    // drain even when the limit is 1. queued_count_ is updated only on strand_;
    // this fast-path read is intentionally approximate.
    const size_t current = queued_count_.load();
    if (max_send_queue_ > 0 && current >= max_send_queue_) {
        if (error) {
            *error = "session_send_queue_full";
        }
        return false;
    }

    auto self = shared_from_this();
    auto frame = std::make_shared<std::vector<uint8_t>>(data);
    boost::asio::post(strand_, [self, frame]() {
        if (!self->alive_.load()) {
            return;
        }
        self->queued_count_.fetch_add(1);
        self->send_queue_.push_back(std::move(*frame));
        if (!self->send_in_progress_) {
            self->send_in_progress_ = true;
            self->do_async_write();
        }
    });

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

    return send(encoded, error);
}

void TcpSession::do_async_write() {
    // Always executed on strand_.
    if (!alive_.load() || send_queue_.empty()) {
        send_in_progress_ = false;
        return;
    }

    auto& front = send_queue_.front();
    boost::asio::async_write(
        socket_, boost::asio::buffer(front),
        boost::asio::bind_executor(
            strand_, [self = shared_from_this()](
                         const boost::system::error_code& ec, std::size_t) {
                if (!self->alive_.load() ||
                    ec == boost::asio::error::operation_aborted) {
                    // close() owns teardown; nothing to do here.
                    return;
                }
                if (ec) {
                    self->handle_error("send error: " + ec.message());
                    return;
                }
                self->queued_count_.fetch_sub(1);
                self->send_queue_.pop_front();
                self->do_async_write();
            }));
}

void TcpSession::close(std::string reason) {
    bool expected = true;
    if (!alive_.compare_exchange_strong(expected, false)) return;

    boost::system::error_code ec;
    socket_.close(ec);        // aborts pending async_read_some / async_write
    try {
        read_deadline_.cancel();
    } catch (...) {
    }

    // Drop queued sends on the strand so any in-flight write completes with
    // operation_aborted rather than touching freed state.
    auto self = shared_from_this();
    boost::asio::post(strand_, [self]() {
        self->send_queue_.clear();
        self->queued_count_.store(0);
        self->send_in_progress_ = false;
    });

    if (callbacks_.on_disconnect) {
        callbacks_.on_disconnect(shared_from_this(), reason);
    }
}

void TcpSession::do_receive() {
    if (!alive_.load()) return;

    receive_buffer_.resize(64 * 1024);

    if (read_idle_timeout_ms_ > 0) {
        read_deadline_.expires_after(
            std::chrono::milliseconds(read_idle_timeout_ms_));
        read_deadline_.async_wait(boost::asio::bind_executor(
            strand_, [self = shared_from_this()](
                         const boost::system::error_code& ec) {
                if (ec) return;  // cancelled or shutting down
                if (self->alive_.load()) {
                    self->handle_error("read idle timeout");
                }
            }));
    }

    socket_.async_read_some(
        boost::asio::buffer(receive_buffer_),
        boost::asio::bind_executor(
            strand_, [self = shared_from_this()](
                         const boost::system::error_code& ec,
                         std::size_t bytes_read) {
                if (self->read_idle_timeout_ms_ > 0) {
                    try {
                        self->read_deadline_.cancel();
                    } catch (...) {
                    }
                }
                if (ec) {
                    if (ec == boost::asio::error::operation_aborted) {
                        return;  // close() in progress
                    }
                    self->handle_error("receive error: " + ec.message());
                    return;
                }

                self->receive_buffer_.resize(bytes_read);

                if (self->protocol_pipeline_) {
                    auto results =
                        self->protocol_pipeline_->feed(
                            self->receive_buffer_.data(),
                            self->receive_buffer_.size());
                    if (!self->protocol_pipeline_->error().empty()) {
                        self->handle_error("protocol decode error: " +
                                           self->protocol_pipeline_->error());
                        return;
                    }

                    for (auto& result : results) {
                        if (!result.ok()) {
                            self->handle_error("protocol dispatch error: " +
                                               result.error);
                            return;
                        }
                        if (result.should_drop()) {
                            continue;
                        }
                        if (result.action ==
                                shield::transport::RouteAction::DecodeLocal &&
                            !self->protocol_pipeline_->materialize_decode(
                                result)) {
                            self->handle_error("protocol decode error: " +
                                               result.error);
                            return;
                        }
                        if (self->callbacks_.on_packet) {
                            self->callbacks_.on_packet(
                                self->shared_from_this(), result);
                        }
                    }
                } else {
                    auto frames = self->frame_decoder_.feed(
                        self->receive_buffer_.data(),
                        self->receive_buffer_.size());
                    if (!self->frame_decoder_.error().empty()) {
                        self->handle_error("frame decode error: " +
                                           self->frame_decoder_.error());
                        return;
                    }

                    for (const auto& frame : frames) {
                        if (self->callbacks_.on_message) {
                            self->callbacks_.on_message(
                                self->shared_from_this(), frame.payload());
                        }
                    }
                }

                self->do_receive();
            }));
}

void TcpSession::handle_error(std::string reason) {
    auto& log = shield::log::get_logger("net");
    SHIELD_LOG_ERROR(log,
                     "Session " + std::to_string(id_) + " error: " + reason);
    // Map error reason to stable error code.
    if (reason.find("idle") != std::string::npos) {
        error_code_ = "read_idle_timeout";
    } else if (reason.find("decode") != std::string::npos ||
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
