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

#include "shield/net/ip_blocklist.hpp"
#include "shield/net/listener_registry.hpp"
#include "shield/net/session.hpp"

namespace shield::net {

/// @brief TCP Listener
class TcpListener {
public:
    TcpListener(boost::asio::io_context& io_context, uint16_t port,
                SessionCallbacks callbacks);

    /// @brief Unregister from the process-wide listener registry. Does not
    ///        stop accepting or close sessions — call stop() first.
    ~TcpListener();

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

    /// @brief Set max frame payload size in bytes (0 = transport-layer
    ///        default cap of 16 MiB, see kDefaultMaxFrameSize)
    void set_max_frame_size(size_t max) { max_frame_size_ = max; }

    /// @brief Set max queued send messages per session (0 = unlimited)
    void set_max_send_queue(size_t max) { max_send_queue_ = max; }

    /// @brief Set per-session read idle timeout in ms (0 = disabled)
    void set_read_idle_timeout(uint32_t ms) { read_idle_timeout_ms_ = ms; }

    /// @brief Set the per-connection ingress rate limit handed to every
    ///        session this listener creates (0 = unlimited).
    void set_rate_limit(uint32_t per_second, uint32_t burst) {
        rate_limit_per_second_ = per_second;
        rate_limit_burst_ = burst;
    }

    /// @brief Install the address blocklist evaluated at accept time.
    ///        Rejected peers never get a session object.
    /// @return false when an entry is malformed; the previous list is kept.
    bool set_blocklist(const std::vector<std::string>& entries,
                       std::string* error = nullptr) {
        return blocklist_.set_rules(entries, error);
    }

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

    /// @brief Gateway observability: connections that completed the TCP
    ///        accept step, including ones rejected right after by the
    ///        blocklist or the connection limits.
    uint64_t accepts_total() const {
        return accepts_total_.load(std::memory_order_relaxed);
    }

    /// @brief Peers rejected by the accept-time address blocklist.
    uint64_t blocked_rejects_total() const {
        return blocked_rejects_total_.load(std::memory_order_relaxed);
    }

    /// @brief Peers rejected because the connection limit was reached.
    uint64_t conn_limit_rejects_total() const {
        return conn_limit_rejects_total_.load(std::memory_order_relaxed);
    }

    /// @brief Peers rejected because the per-IP limit was reached.
    uint64_t ip_limit_rejects_total() const {
        return ip_limit_rejects_total_.load(std::memory_order_relaxed);
    }

    /// @brief Ingress messages dropped by the per-connection rate limit
    ///        across every session this listener ever created (cumulative,
    ///        survives session exit — a Prometheus-clean counter).
    uint64_t rate_limited_messages_total() const {
        return rate_limited_total_.load(std::memory_order_relaxed);
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
    size_t max_frame_size_ = 0;          // 0 = kDefaultMaxFrameSize (16 MiB)
    size_t max_send_queue_ = 0;          // 0 = unlimited (queued message count)
    uint32_t read_idle_timeout_ms_ = 0;  // 0 = disabled
    uint32_t rate_limit_per_second_ = 0;  // 0 = unlimited
    uint32_t rate_limit_burst_ = 0;       // 0 = same as the rate
    // Accept-time address blocklist. Consulted before a session is built, so
    // a blocked peer costs one address comparison and nothing else.
    IpBlocklist blocklist_;
    std::unordered_map<std::string, size_t> ip_counts_;
    std::string last_rejection_;
    bool listening_ = false;

    // Gateway observability counters (relaxed: stats-only, no ordering
    // expectations across listeners or with the session map).
    std::atomic<uint64_t> accepts_total_{0};
    std::atomic<uint64_t> blocked_rejects_total_{0};
    std::atomic<uint64_t> conn_limit_rejects_total_{0};
    std::atomic<uint64_t> ip_limit_rejects_total_{0};
    std::atomic<uint64_t> rate_limited_total_{0};

    static std::atomic<SessionId> g_next_session_id;
};

}  // namespace shield::net
