// [SHIELD_NET] Listener implementation
#include "shield/net/listener.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>
#ifdef _WIN32
#include <WinSock2.h>
#endif
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "shield/log/logger.hpp"

namespace shield::net {

// Per-connection TLS handshake state machine (docs/tls-design.md). Heap-owned
// and self-referential: it lives exactly as long as its pending completions.
// Everything runs on a dedicated per-connection strand so the timer expiry
// and the handshake completion can never race each other, whichever thread
// of the net pool they land on. First side to settle wins: it closes the
// socket, records the counter and logs; the other side observes `settled`
// and walks away.
//
// Defined in shield::net rather than an anonymous namespace: TcpListener
// declares it a friend by name, and the anonymous-namespace spelling would be
// a different type.
struct TlsHandshakeState : std::enable_shared_from_this<TlsHandshakeState> {
    TlsHandshakeState(TcpListener* owner, std::unique_ptr<TlsStream> tls_stream,
                      std::shared_ptr<TcpListener::HandshakeGate> gate,
                      std::shared_ptr<TcpListener::HandshakeCounters> counters,
                      std::string peer_ip)
        : listener(owner),
          stream(std::move(tls_stream)),
          gate(std::move(gate)),
          strand(
              boost::asio::make_strand(stream->lowest_socket().get_executor())),
          deadline(strand),
          counters(std::move(counters)),
          remote_ip(std::move(peer_ip)) {}

    void run(uint32_t timeout_ms) {
        auto self = shared_from_this();
        deadline.expires_after(std::chrono::milliseconds(timeout_ms));
        deadline.async_wait(boost::asio::bind_executor(
            strand, [self](const boost::system::error_code& ec) {
                self->on_timeout(ec);
            }));
        stream->raw().async_handshake(
            boost::asio::ssl::stream_base::server,
            boost::asio::bind_executor(
                strand, [self](const boost::system::error_code& ec) {
                    self->on_handshake(ec);
                }));
    }

    /// @brief Called by the listener while it tears down (C1). Sets the
    ///        abort flag, cancels the deadline and closes the TLS socket.
    ///        The socket close is thread-safe with respect to the in-flight
    ///        handshake, so any completion already queued on the strand
    ///        observes `cancelled` before touching the listener; a state
    ///        that already settled is left alone (the adopt path owns it and
    ///        checks `stopping_` itself).
    void cancel() {
        if (settled.load(std::memory_order_acquire)) {
            return;
        }
        cancelled.store(true, std::memory_order_release);
        deadline.cancel();
        boost::system::error_code close_ec;
        // Serialize the socket close with a concurrent settle() (timeout /
        // handshake-failure completion on the strand): Boost.Asio does not
        // guarantee that two threads calling close() on the same socket
        // object are safe, and teardown's cancel() runs off-strand.
        std::lock_guard lock(close_mutex_);
        stream->lowest_socket().close(close_ec);
    }

    void on_timeout(const boost::system::error_code& ec) {
        if (cancelled.load(std::memory_order_acquire)) {
            return;
        }
        if (ec) {
            return;  // cancelled: the handshake side owns the outcome
        }
        settle("TLS handshake timeout");
    }

    void on_handshake(const boost::system::error_code& ec) {
        try {
            deadline.cancel();
        } catch (...) {  // GCOVR_EXCL_LINE (cancel does not throw)
        }  // GCOVR_EXCL_LINE
        if (ec == boost::asio::error::operation_aborted) {
            return;  // listener teardown path; the timeout side owns teardown
        }
        if (cancelled.load(std::memory_order_acquire)) {
            return;
        }
        if (ec) {
            settle("TLS handshake failed: " + ec.message());
            return;
        }
        // Serialize with listener teardown through the gate, which we hold
        // by shared_ptr and which therefore outlives both the listener and
        // this state. If teardown already set stopping, the listener is
        // going away: never dereference it. If we win the mutex first,
        // teardown blocks until adoption completes, so the listener cannot
        // be freed underneath us.
        std::lock_guard lock(gate->mutex);
        if (gate->stopping) {
            return;
        }
        settled.store(true, std::memory_order_release);
        counters->handshakes.fetch_add(1, std::memory_order_relaxed);
        listener->adopt_connection(std::move(stream), remote_ip);
    }

    /// @brief Record the failure/timeout, close the peer and log. Exactly one
    /// caller reaches here per state (the other observed `settled` or was
    /// cancelled).
    void settle(const std::string& why) {
        const bool already_settled =
            settled.exchange(true, std::memory_order_acq_rel);
        if (already_settled) {  // GCOVR_EXCL_BR_LINE (defensive: the strand
                                // serializes both completions, so the
                                // double-settle arm is only reachable in the
                                // timer/handshake completion race window)
            // GCOVR_EXCL_START (defensive: see the branch above — no test
            // can reach this arm deterministically)
            return;
            // GCOVR_EXCL_STOP
        }
        counters->failures.fetch_add(1, std::memory_order_relaxed);
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_WARNING(log, why + " (" + remote_ip + ")");
        boost::system::error_code close_ec;
        std::lock_guard lock(close_mutex_);
        stream->lowest_socket().close(close_ec);
    }

    TcpListener* listener;
    std::unique_ptr<TlsStream> stream;
    std::shared_ptr<TcpListener::HandshakeGate> gate;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    boost::asio::steady_timer deadline;
    std::shared_ptr<TcpListener::HandshakeCounters> counters;
    std::string remote_ip;
    std::atomic<bool> settled{false};
    std::atomic<bool> cancelled{false};
    std::mutex close_mutex_;
};

std::atomic<SessionId> TcpListener::g_next_session_id{1};

TcpListener::TcpListener(boost::asio::io_context& io_context, uint16_t port,
                         SessionCallbacks callbacks)
    : io_context_(io_context),
      acceptor_(io_context),
      port_(port),
      callbacks_(std::move(callbacks)),
      socket_(io_context) {
    boost::system::error_code ec;

    // Open acceptor
    acceptor_.open(boost::asio::ip::tcp::v4(), ec);
    // GCOVR_EXCL_START (only fails on fd exhaustion; not testable here)
    if (ec) {
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_ERROR(log, "Failed to open acceptor: " + ec.message());
        return;
    }
    // GCOVR_EXCL_STOP

// On Windows, SO_REUSEADDR allows hijacking an already-bound port. Use
// SO_EXCLUSIVEADDRUSE instead, which gives Linux-like behaviour: the
// bind will fail if another socket is already listening on the same port.
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    ::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive),
                 static_cast<int>(sizeof(exclusive)));
#else
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
#endif

    // Bind to port
    acceptor_.bind(
        boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port), ec);

    if (ec) {
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_ERROR(log, "Failed to bind to port " + std::to_string(port) +
                                  ": " + ec.message());
        return;
    }

    // Start listening
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);

    // GCOVR_EXCL_START (listen after a successful bind only fails on
    // exotic kernel-level errors; not testable here)
    if (ec) {
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_ERROR(log, "Failed to listen: " + ec.message());
        return;
    }
    // GCOVR_EXCL_STOP

    listening_ = true;
    ListenerRegistry::instance().add(this);
}  // GCOVR_EXCL_LINE (uncalled exit clone)

TcpListener::~TcpListener() {
    stopping_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(handshake_gate_->mutex);
        handshake_gate_->stopping = true;
    }
    cancel_in_flight_handshakes();
    ListenerRegistry::instance().remove(this);
}

void TcpListener::start() {
    if (!listening_) {
        return;
    }
    do_accept();
}

void TcpListener::stop() {
    stopping_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(handshake_gate_->mutex);
        handshake_gate_->stopping = true;
    }
    cancel_in_flight_handshakes();
    boost::system::error_code ec;
    acceptor_.close(ec);
    listening_ = false;

    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::unique_lock lock(sessions_mutex_);
        for (auto& [id, session] : sessions_) {
            sessions.push_back(session);
        }
        sessions_.clear();
        ip_counts_.clear();
    }

    for (auto& session : sessions) {
        session->close("listener shutdown");
    }
}

void TcpListener::cancel_in_flight_handshakes() {
    // Snapshot strong refs under the lock (cancel() never calls back into
    // this listener), then cancel outside it.
    std::vector<std::shared_ptr<TlsHandshakeState>> states;
    {
        std::lock_guard lock(handshakes_mutex_);
        for (auto it = in_flight_handshakes_.begin();
             it != in_flight_handshakes_.end();) {
            if (auto locked = it->lock()) {
                states.push_back(std::move(locked));
                ++it;
            } else {
                it = in_flight_handshakes_.erase(it);
            }
        }
    }
    for (auto& state : states) {
        state->cancel();
    }
}

void TcpListener::do_accept() {
    // stop() may close the acceptor and clear listening_ while a previous
    // accept completion is still draining (e.g. a rejection path that calls
    // do_accept() again). The guard prevents re-arming the accept loop on a
    // closed acceptor, which would otherwise spin on operation_aborted or
    // re-submit an accept on a stopped listener.
    if (!listening_) {
        return;
    }
    auto gate = handshake_gate_;
    acceptor_.async_accept(socket_, [this, gate](auto ec) {
        // C1: teardown (stop()/dtor) may run on another thread while this
        // accept completion is queued. The gate outlives the listener, so
        // the fast path bails before touching `this`; the slow path takes
        // the gate mutex, which blocks teardown until this completion is
        // done — the listener cannot be freed underneath us (mirrors
        // TlsHandshakeState::on_handshake).
        if (gate->stopping) {
            return;
        }
        std::lock_guard lock(gate->mutex);
        if (gate->stopping) {
            return;
        }
        if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
                auto& log = shield::log::get_logger("net");
                SHIELD_LOG_ERROR(log, "Accept error: " + ec.message());
                // Re-arm the accept loop. A transient accept failure
                // (EMFILE / ENFILE / ECONNABORTED / ENOBUFS) must not kill
                // the listener: without the re-arm the acceptor goes idle
                // forever while the process keeps running. operation_aborted
                // is the only permanent stop (issued by stop()).
                do_accept();
            }
            return;
        }

        // error_code overload: a peer that vanished between SYN and accept
        // completion leaves the socket dead (bad descriptor on macOS's
        // reactive service), and the throwing overload would abort the io
        // loop out of this handler. The failure arm is defensive: on Linux
        // an accepted socket is already established before the completion
        // handler runs, so no test can make remote_endpoint() report an
        // error deterministically — the arm only fires in a race window on
        // reactive platforms.
        boost::system::error_code remote_ec;
        const auto remote_ep = socket_.remote_endpoint(remote_ec);
        // GCOVR_EXCL_START (defensive: dead-peer-at-accept race window, see
        // the comment above — not drivable from a test)
        if (remote_ec) {
            auto& log = shield::log::get_logger("net");
            SHIELD_LOG_WARNING(
                log, std::string("Accepted connection already dead: ") +
                         remote_ec.message());
            boost::system::error_code close_ec;
            socket_.close(close_ec);
            do_accept();
            return;
        }
        // GCOVR_EXCL_STOP
        std::string remote_ip = remote_ep.address().to_string();

        accepts_total_.fetch_add(1, std::memory_order_relaxed);

        // Blocked addresses are rejected first: no session object, no
        // per-session state, and the connection-count bookkeeping below is
        // never touched for them.
        if (blocklist_.blocked(remote_ep.address())) {
            blocked_rejects_total_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard lock(rejection_mutex_);
                last_rejection_ = "blocked_ip";
            }
            auto& log = shield::log::get_logger("net");
            SHIELD_LOG_WARNING(log, "Connection rejected: address blocked (" +
                                        remote_ip + ")");
            boost::system::error_code close_ec;
            socket_.close(close_ec);
            do_accept();
            return;
        }

        // Check connection limit.
        {
            std::unique_lock lock(sessions_mutex_);
            if (max_connections_ > 0 && sessions_.size() >= max_connections_) {
                conn_limit_rejects_total_.fetch_add(1,
                                                    std::memory_order_relaxed);
                {
                    std::lock_guard lock(rejection_mutex_);
                    last_rejection_ = "connection_limit";
                }
                auto& log = shield::log::get_logger("net");
                SHIELD_LOG_WARNING(log, "Connection rejected: limit reached (" +
                                            std::to_string(max_connections_) +
                                            ")");
                boost::system::error_code close_ec;
                socket_.close(close_ec);
                do_accept();
                return;
            }
        }

        // Check per-IP limit.
        {
            std::unique_lock lock(sessions_mutex_);
            if (max_per_ip_ > 0) {
                auto it = ip_counts_.find(remote_ip);
                if (it != ip_counts_.end() && it->second >= max_per_ip_) {
                    ip_limit_rejects_total_.fetch_add(
                        1, std::memory_order_relaxed);
                    {
                        std::lock_guard lock(rejection_mutex_);
                        last_rejection_ = "ip_limit";
                    }
                    auto& log = shield::log::get_logger("net");
                    SHIELD_LOG_WARNING(
                        log, "Connection rejected: IP limit for " + remote_ip +
                                 " (" + std::to_string(max_per_ip_) + ")");
                    boost::system::error_code close_ec;
                    socket_.close(close_ec);
                    do_accept();
                    return;
                }
            }
        }

        // Branch on transport before any per-connection state exists: a TLS
        // listener runs the handshake outside the accept loop (parallel
        // re-accept below keeps the pipeline full), a plain listener builds
        // the session immediately. Both paths share the accept tail.
        if (tls_context_ != nullptr) {
            begin_tls_handshake(std::move(socket_), std::move(remote_ip));
        } else {
            adopt_connection(std::make_unique<PlainStream>(std::move(socket_)),
                             remote_ip);
        }

        // Accept next
        do_accept();
    });
}

void TcpListener::adopt_connection(std::unique_ptr<SessionStream> stream,
                                   std::string remote_ip) {
    // Create session
    SessionId id = g_next_session_id.fetch_add(1);
    SessionCallbacks callbacks = callbacks_;
    auto user_disconnect = callbacks.on_disconnect;
    callbacks.on_disconnect = [this, user_disconnect](
                                  std::shared_ptr<Session> session,
                                  std::string_view reason) {
        on_session_close(session, std::string(reason));
        if (user_disconnect) {
            user_disconnect(std::move(session), reason);
        }
    };
    // Cumulative rate-limit counter for the /ops/metrics gateway view:
    // kept at the listener so it survives session exit.
    auto user_rate_limited = callbacks.on_rate_limited;
    callbacks.on_rate_limited = [this, user_rate_limited] {
        rate_limited_total_.fetch_add(1, std::memory_order_relaxed);
        if (user_rate_limited) {
            user_rate_limited();
        }
    };
    auto session = std::make_shared<TcpSession>(
        id, std::move(stream), std::move(callbacks), max_frame_size_,
        max_send_queue_, read_idle_timeout_ms_, rate_limit_per_second_,
        rate_limit_burst_);

    // Store session
    {
        std::unique_lock lock(sessions_mutex_);
        // stop() sets stopping_ before it takes sessions_mutex_ to snapshot,
        // so once we hold the lock and stopping_ is clear, every later
        // stop()/destructor will see this session in its snapshot. If
        // stopping_ is already set, the listener is tearing down: drop the
        // socket instead of registering a session whose callbacks capture
        // the dying listener.
        if (stopping_.load(std::memory_order_acquire)) {
            boost::system::error_code close_ec;
            stream->lowest_socket().close(close_ec);
            return;
        }
        sessions_[id] = session;
        ++ip_counts_[remote_ip];
    }

    // Start session
    session->start();
}

void TcpListener::begin_tls_handshake(boost::asio::ip::tcp::socket socket,
                                      std::string remote_ip) {
    auto stream = std::make_unique<TlsStream>(std::move(socket), *tls_context_);
    auto state = std::make_shared<TlsHandshakeState>(
        this, std::move(stream), handshake_gate_, tls_counters_,
        std::move(remote_ip));
    {
        std::lock_guard lock(handshakes_mutex_);
        // Prune expired entries so the registry stays bounded by the peak
        // number of concurrent handshakes on a long-lived listener.
        for (auto it = in_flight_handshakes_.begin();
             it != in_flight_handshakes_.end();) {
            if (it->expired()) {
                it = in_flight_handshakes_.erase(it);
            } else {
                ++it;
            }
        }
        in_flight_handshakes_.push_back(state);
    }
    state->run(tls_handshake_timeout_ms_);
}

std::shared_ptr<Session> TcpListener::find_session(SessionId id) const {
    std::shared_lock lock(sessions_mutex_);
    auto it = sessions_.find(id);
    return it != sessions_.end() ? it->second : nullptr;
}

void TcpListener::broadcast(const std::vector<uint8_t>& data) {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::shared_lock lock(sessions_mutex_);
        for (auto& [id, session] : sessions_) {
            sessions.push_back(session);
        }
    }

    for (auto& session : sessions) {
        if (session->is_alive()) {  // GCOVR_EXCL_BR_LINE (defensive: close()
                                    // synchronously removes the session from
                                    // the list via on_disconnect, so a dead
                                    // session only appears here in the
                                    // concurrent-close race window)
            session->send(data);
        }
    }
}

bool TcpListener::kick_session(SessionId id, std::string reason) {
    std::shared_ptr<Session> session;
    {
        std::unique_lock lock(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return false;
        }
        session = it->second;
        remove_session_locked(session);
    }

    session->close(std::move(reason));
    return true;
}

void TcpListener::remove_session_locked(
    const std::shared_ptr<Session>& session) {
    auto it = sessions_.find(session->id());
    if (it != sessions_.end()) {
        // Decrement IP count.
        std::string ip = session->remote_addr().ip;
        auto ip_it = ip_counts_.find(ip);
        if (ip_it !=
            ip_counts_
                .end()) {  // GCOVR_EXCL_BR_LINE (defensive: sessions_ and
                           // ip_counts_ are maintained under the same lock, so
                           // a session hit implies its IP entry exists)
            if (ip_it->second > 1) {
                --ip_it->second;
            } else {
                ip_counts_.erase(ip_it);
            }
        }
        sessions_.erase(it);
    }
}

void TcpListener::on_session_close(std::shared_ptr<Session> session,
                                   std::string reason) {
    std::unique_lock lock(sessions_mutex_);
    remove_session_locked(session);
}

}  // namespace shield::net
