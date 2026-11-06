// [SHIELD_NET] Console session implementation
#include "shield/net/console_session.hpp"

#include <boost/asio.hpp>
#include <boost/asio/read_until.hpp>
#include <iostream>

namespace shield::net {

ConsoleSession::ConsoleSession(uint64_t id, socket_t socket,
                               ConsoleSessionCallbacks callbacks)
    : id_(id),
      socket_(std::move(socket)),
      strand_(boost::asio::make_strand(socket_.get_executor())),
      callbacks_(std::move(callbacks)) {}

ConsoleSession::~ConsoleSession() = default;

void ConsoleSession::start() {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self]() { self->do_read(); });
}

void ConsoleSession::do_read() {
    auto self = shared_from_this();
    boost::asio::async_read_until(
        socket_, read_buf_, '\n',
        boost::asio::bind_executor(
            strand_, [self](boost::system::error_code ec, std::size_t bytes) {
                if (ec) {
                    self->handle_close();
                    return;
                }

                // Extract lines from the buffer
                auto bufs = self->read_buf_.data();
                std::string data(boost::asio::buffers_begin(bufs),
                                 boost::asio::buffers_begin(bufs) + bytes);
                self->read_buf_.consume(bytes);

                // Process each complete line
                std::string::size_type pos = 0;
                while (pos < data.size()) {
                    auto nl = data.find('\n', pos);
                    if (nl == std::string::npos)  // GCOVR_EXCL_BR_LINE
                        break;  // GCOVR_EXCL_LINE (defensive: async_read_until
                                // guarantees a '\n' within the reported bytes,
                                // so the delimiter is never missing)
                    std::string line = data.substr(pos, nl - pos);
                    // Strip trailing \r if present
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    pos = nl + 1;
                    if (self->callbacks_.on_line) {
                        self->callbacks_.on_line(self, std::move(line));
                    }
                }

                // Continue reading
                self->do_read();
            }));
}

void ConsoleSession::send_line(const std::string& line) {
    auto msg = std::make_shared<std::string>(line + "\n");
    // Capture shared_from_this() so the session stays alive until the posted
    // write runs. The session is managed by a shared_ptr in ConsoleServer and
    // may be erased from sessions_ (e.g. on close) while the write is still
    // queued; a raw `this` would then be a use-after-free.
    auto self = shared_from_this();
    boost::asio::post(
        strand_,
        [self, msg]() {  // GCOVR_EXCL_BR_LINE (compiler artifact: the inlined
                         // lambda's return/exception pseudo-branch is
                         // attributed to the lambda opening line)
            // Defensive: strand serialization implies an empty queue means the
            // prior write finished and reset write_in_progress_, so the
            // both-true arm of the idle computation is unreachable.
            bool idle =
                self->send_queue_.empty() &&
                !self->write_in_progress_;  // GCOVR_EXCL_BR_LINE (defensive:
                                            // see note above — the
                                            // write_in_progress_ true arm is
                                            // unreachable)
            self->send_queue_.push_back(std::move(*msg));
            if (idle) {
                self->do_write();
            }
        });
}

void ConsoleSession::do_write() {
    if (send_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }
    write_in_progress_ = true;
    auto self = shared_from_this();
    auto& front = send_queue_.front();
    boost::asio::async_write(
        socket_, boost::asio::buffer(front),
        boost::asio::bind_executor(strand_, [self](boost::system::error_code ec,
                                                   std::size_t /*bytes*/) {
            if (ec) {
                self->handle_close();
                return;
            }
            self->send_queue_.pop_front();
            self->do_write();
        }));
}

void ConsoleSession::close() {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self]() {
        // GCOVR_EXCL_BR_START (CI-only artifact: GCC inlines this lambda so
        // a pseudo-arc of the idempotent double-close guard lands here; the
        // real arms are driven by the double-close suite)
        if (!self->alive_.exchange(false)) return;
        // GCOVR_EXCL_BR_STOP
        boost::system::error_code ec;
        self->socket_.close(ec);
    });
}

void ConsoleSession::handle_close() {
    // GCOVR_EXCL_BR_START (CI-only artifact: newer GCC on the Coverage job
    // inlines a std::function invocation copy into the on_close check whose
    // never-taken arm lands on the same line; the real arms are covered by
    // the close-path suites)
    if (!alive_.exchange(false)) return;
    boost::system::error_code ec;
    socket_.close(ec);
    if (callbacks_.on_close) {
        callbacks_.on_close(shared_from_this());
    }
    // GCOVR_EXCL_BR_STOP
}

}  // namespace shield::net
