// [SHIELD_NET] Console session for diagnostics Unix socket
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace shield::net {

/// @brief Console session callbacks
struct ConsoleSessionCallbacks {
    /// @brief Called when a complete line is received (without trailing \n)
    std::function<void(std::shared_ptr<class ConsoleSession>, std::string)>
        on_line;
    /// @brief Called when the session is closed
    std::function<void(std::shared_ptr<class ConsoleSession>)> on_close;
};

/// @brief A single console client session over a Unix domain socket.
///
/// Reads newline-delimited text lines and delivers them via on_line callback.
/// Provides send_line() for sending JSON responses back.
class ConsoleSession
    : public std::enable_shared_from_this<ConsoleSession> {
public:
    using socket_t = boost::asio::local::stream_protocol::socket;

    ConsoleSession(uint64_t id, socket_t socket,
                   ConsoleSessionCallbacks callbacks);
    ~ConsoleSession();

    /// @brief Start reading
    void start();

    /// @brief Send a line (appends \n automatically)
    void send_line(const std::string& line);

    /// @brief Close the session
    void close();

    /// @brief Check if session is alive
    bool is_alive() const { return alive_.load(); }

    /// @brief Get session ID
    uint64_t id() const { return id_; }

    /// @brief Get attached Lua service name (empty if in command mode)
    const std::string& attached_service() const { return attached_service_; }

    /// @brief Set attached Lua service name
    void set_attached_service(const std::string& service) {
        attached_service_ = service;
    }

    /// @brief Get multiline Lua input buffer
    const std::string& multiline_buffer() const { return multiline_buf_; }

    /// @brief Append to multiline buffer
    void append_multiline(const std::string& line) { multiline_buf_ += line; }

    /// @brief Clear multiline buffer
    void clear_multiline() { multiline_buf_.clear(); }

    /// @brief True if attached to a Lua service
    bool is_attached() const { return !attached_service_.empty(); }

private:
    void do_read();
    void do_write();
    void handle_close();

    uint64_t id_;
    socket_t socket_;
    boost::asio::any_io_executor strand_;
    boost::asio::streambuf read_buf_;
    std::deque<std::string> send_queue_;
    bool write_in_progress_{false};
    std::atomic<bool> alive_{true};
    ConsoleSessionCallbacks callbacks_;

    // Lua REPL state
    std::string attached_service_;
    std::string multiline_buf_;
};

}  // namespace shield::net
