// [SHIELD_NET] Listener types
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
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
#include "shield/net/session_stream.hpp"

namespace shield::net {

struct TlsHandshakeState;

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
    /// @note In-flight TLS handshakes are not force-closed here; each one
    ///       carries its own deadline and self-closes, so io_context::run()
    ///       drains within handshake_timeout_ms of the last accept.
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

    /// @brief Serve TLS on this listener: every accepted connection runs a
    /// server-side handshake (bounded by @p handshake_timeout_ms) before a
    /// session exists, and failed/timed-out handshakes never produce one.
    /// A listener without set_tls() serves plaintext exactly as before.
    /// Call before start(); enabling mid-flight only affects later accepts.
    void set_tls(std::shared_ptr<boost::asio::ssl::context> context,
                 uint32_t handshake_timeout_ms) {
        tls_context_ = std::move(context);
        tls_handshake_timeout_ms_ = handshake_timeout_ms;
    }

    /// @brief True when set_tls() installed a context (this port is pure
    ///        TLS: plaintext peers are rejected, never downgraded).
    bool tls_enabled() const { return tls_context_ != nullptr; }

    /// @brief TLS handshakes that completed successfully on this listener.
    uint64_t tls_handshakes_total() const {
        return tls_counters_->handshakes.load(std::memory_order_relaxed);
    }

    /// @brief TLS handshakes that failed or timed out (no session built).
    uint64_t tls_handshake_failures_total() const {
        return tls_counters_->failures.load(std::memory_order_relaxed);
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
    friend struct TlsHandshakeState;

    void do_accept();

    /// @brief Shared accept tail for both transports: wrap callbacks, build
    /// the session around @p stream, register it and start it.
    void adopt_connection(std::unique_ptr<SessionStream> stream,
                          std::string remote_ip);

    /// @brief Hand the accepted socket to the per-connection handshake state
    /// machine (heap-owned; completes independently of the accept loop).
    void begin_tls_handshake(boost::asio::ip::tcp::socket socket,
                             std::string remote_ip);

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

    // TLS state. tls_context_ is null for a plaintext listener. The
    // handshake counters live behind a shared_ptr so in-flight per-connection
    // handshake states can bump them even if the listener is being torn down
    // underneath them.
    struct HandshakeCounters {
        std::atomic<uint64_t> handshakes{0};
        std::atomic<uint64_t> failures{0};
    };
    std::shared_ptr<boost::asio::ssl::context> tls_context_;
    uint32_t tls_handshake_timeout_ms_ = 0;  // 0 = TLS disabled
    std::shared_ptr<HandshakeCounters> tls_counters_ =
        std::make_shared<HandshakeCounters>();

    static std::atomic<SessionId> g_next_session_id;
};

}  // namespace shield::net
