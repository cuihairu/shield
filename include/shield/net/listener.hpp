// [SHIELD_NET] Listener types
#pragma once

#include "shield/net/session.hpp"

#include <boost/asio.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace shield::net {

/// @brief TCP Listener
class TcpListener {
public:
    TcpListener(boost::asio::io_context& io_context,
               uint16_t port,
               SessionCallbacks callbacks);

    /// @brief Start accepting connections
    void start();

    /// @brief Stop accepting connections
    void stop();

    /// @brief Get listen port
    uint16_t port() const { return port_; }

    /// @brief Get number of active sessions
    size_t session_count() const {
        std::shared_lock lock(sessions_mutex_);
        return sessions_.size();
    }

    /// @brief Find session by ID
    std::shared_ptr<Session> find_session(SessionId id) const;

    /// @brief Broadcast to all sessions
    void broadcast(const std::vector<uint8_t>& data);

    /// @brief Kick a session
    bool kick_session(SessionId id, std::string reason);

private:
    void do_accept();

    void on_session_close(std::shared_ptr<Session> session,
                         std::string reason);

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    SessionCallbacks callbacks_;

    boost::asio::ip::tcp::socket socket_;
    std::unordered_map<SessionId, std::shared_ptr<Session>> sessions_;
    mutable std::shared_mutex sessions_mutex_;

    static std::atomic<SessionId> g_next_session_id;
};

}  // namespace shield::net
