// [SHIELD_NET] Listener types
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "shield/net/session.hpp"

namespace shield::net {

/// @brief TCP Listener
class TcpListener {
public:
    TcpListener(boost::asio::io_context& io_context, uint16_t port,
                SessionCallbacks callbacks);

    /// @brief Start accepting connections
    void start();

    /// @brief Stop accepting connections
    void stop();

    /// @brief Get listen port
    uint16_t port() const { return port_; }

    /// @brief Set max connections (0 = unlimited)
    void set_max_connections(size_t max) { max_connections_ = max; }

    /// @brief Set max connections per IP (0 = unlimited)
    void set_max_per_ip(size_t max) { max_per_ip_ = max; }

    /// @brief Set max frame payload size in bytes (0 = unlimited)
    void set_max_frame_size(size_t max) { max_frame_size_ = max; }

    /// @brief Set max queued send messages per session (0 = unlimited)
    void set_max_send_queue(size_t max) { max_send_queue_ = max; }

    /// @brief Set per-session read idle timeout in ms (0 = disabled)
    void set_read_idle_timeout(uint32_t ms) { read_idle_timeout_ms_ = ms; }

    /// @brief Get last rejection reason
    std::string last_rejection_reason() const { return last_rejection_; }

    /// @brief True when the underlying acceptor was opened, bound and
    /// listening.
    bool is_open() const { return listening_; }

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

    void remove_session_locked(const std::shared_ptr<Session>& session);

    void on_session_close(std::shared_ptr<Session> session, std::string reason);

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    SessionCallbacks callbacks_;

    boost::asio::ip::tcp::socket socket_;
    std::unordered_map<SessionId, std::shared_ptr<Session>> sessions_;
    mutable std::shared_mutex sessions_mutex_;
    size_t max_connections_ = 0;         // 0 = unlimited
    size_t max_per_ip_ = 0;              // 0 = unlimited
    size_t max_frame_size_ = 0;          // 0 = unlimited
    size_t max_send_queue_ = 0;          // 0 = unlimited (queued message count)
    uint32_t read_idle_timeout_ms_ = 0;  // 0 = disabled
    std::unordered_map<std::string, size_t> ip_counts_;
    std::string last_rejection_;
    bool listening_ = false;

    static std::atomic<SessionId> g_next_session_id;
};

}  // namespace shield::net
