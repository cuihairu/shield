// [SHIELD_NET] Console server - Unix domain socket listener for diagnostics
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "shield/net/console_session.hpp"

namespace shield::net {

/// @brief Console server that listens on a Unix domain socket for diagnostic
/// connections.
///
/// Each accepted connection gets a ConsoleSession that delivers
/// newline-delimited text commands. On Windows, falls back to a TCP loopback
/// socket.
class ConsoleServer {
public:
    /// @param io_context Shared network I/O context
    /// @param socket_path Path for the Unix socket (e.g. "/tmp/shield.sock")
    ConsoleServer(boost::asio::io_context& io_context,
                  const std::string& socket_path);
    ~ConsoleServer();

    /// @brief Bind and start accepting connections
    void start();

    /// @brief Stop accepting and close all sessions
    void stop();

    /// @brief Number of active console sessions
    size_t session_count() const;

    /// @brief Set the line handler for new sessions.
    ///
    /// Called for each line received from any console session.
    /// Must be called before start().
    void set_on_line(
        std::function<void(std::shared_ptr<ConsoleSession>, std::string)>
            on_line) {
        on_line_ = std::move(on_line);
    }

    /// @brief Close all connected sessions
    void close_all_sessions();

private:
    void do_accept();

    boost::asio::io_context& io_;
#ifndef _WIN32
    boost::asio::local::stream_protocol::acceptor acceptor_;
#else
    boost::asio::ip::tcp::acceptor acceptor_;
#endif
    std::string socket_path_;
    bool listening_{false};

    std::function<void(std::shared_ptr<ConsoleSession>, std::string)> on_line_;

    std::vector<std::shared_ptr<ConsoleSession>> sessions_;
    mutable std::mutex sessions_mutex_;
    std::atomic<uint64_t> next_session_id_{1};
};

}  // namespace shield::net
