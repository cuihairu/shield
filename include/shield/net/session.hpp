// [SHIELD_NET] Session types
#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shield/transport/protocol.hpp"

namespace shield::net {

/// @brief Session ID
using SessionId = uint64_t;

/// @brief Remote address
struct RemoteAddress {
    std::string ip;
    uint16_t port;

    std::string to_string() const { return ip + ":" + std::to_string(port); }
};

/// @brief Session close reasons
namespace CloseReason {
constexpr const char* NORMAL = "normal";
constexpr const char* TIMEOUT = "timeout";
constexpr const char* ERROR_REASON = "error";
constexpr const char* KICKED = "kicked";
constexpr const char* SHUTDOWN = "shutdown";
}  // namespace CloseReason

/// @brief Epoch sentinel for apply_binding(): skip the staleness check and
/// replace the binding unconditionally (used when invalidating a session).
inline constexpr uint32_t kAnyEpoch = 0xFFFFFFFFu;

/// @brief Per-connection ingress rate limiter (token bucket).
///
/// One bucket per session, created by the listener from the owning actor's
/// `network.rate_limit` config. Every decoded ingress message costs one token;
/// a message arriving with an empty bucket is counted as rate-limited and
/// dropped before dispatch (the connection stays open — a client that bursts
/// past its budget must not lose its session, see runtime-security.md).
///
/// The bucket starts full so a burst of up to `burst` messages is admitted
/// immediately, then refills continuously at `rate_per_second`. Refill is
/// computed lazily on each try_acquire() from an elapsed-time delta, so an
/// idle session costs nothing and there is no timer thread.
class TokenBucket {
public:
    /// @param rate_per_second Sustained refill rate (0 = disabled; every
    ///                       try_acquire() then returns true).
    /// @param burst Bucket depth; 0 is normalized to the rate so a plain
    ///              "N per second" config needs no second knob.
    TokenBucket(uint32_t rate_per_second, uint32_t burst);

    /// @brief Disabled buckets admit everything.
    bool enabled() const { return rate_per_second_ > 0; }

    /// @brief Spend one token. False means the message must be dropped.
    bool try_acquire();

    /// @brief Number of messages rejected since construction.
    uint64_t limited_count() const { return limited_count_; }

    /// @brief Current token count (fractional part truncated). Test/diagnostic
    ///        accessor; the value is only meaningful on the owning strand.
    uint32_t available() const { return static_cast<uint32_t>(tokens_); }

private:
    uint32_t rate_per_second_ = 0;
    double burst_ = 0;
    double tokens_ = 0;
    // Accumulated refill credit in seconds; keeping the remainder means a slow
    // drip of low-rate buckets still refills exactly, with no drift.
    double refill_credit_ = 0;
    std::chrono::steady_clock::time_point last_refill_;
    uint64_t limited_count_ = 0;
};

/// @brief Single-target binding of a live session. The gateway keeps exactly
/// one target service per session: the listener's auth entry service before
/// login, the player service after. Every successful apply_binding()
/// increments epoch, so stale ingress/egress/control references carrying an
/// older epoch are rejected.
struct SessionBinding {
    std::string target_service;  // auth service pre-login, player service after
    std::string player_id;       // empty until authenticated
    std::string gateway_name;    // gateway actor / listener owner
    std::string protocol_profile_id;
    uint32_t epoch = 0;  // incremented on every binding replacement
};

/// @brief Session interface
class Session {
public:
    virtual ~Session() = default;  // GCOVR_EXCL_LINE (dtor clones)

    /// @brief Get session ID
    virtual SessionId id() const = 0;

    /// @brief Get remote address
    virtual RemoteAddress remote_addr() const = 0;

    /// @brief Enqueue data for asynchronous send. Returns true when the data
    /// was accepted into the send queue; false (with @p error populated) when
    /// the session is closed or the send queue is full (backpressure). This
    /// never blocks on the socket.
    virtual bool send(const std::vector<uint8_t>& data,
                      std::string* error = nullptr) = 0;

    /// @brief Close session
    virtual void close(std::string reason) = 0;

    /// @brief Check if session is alive
    virtual bool is_alive() const = 0;

    /// @brief Get the last error code (for Lua API error mapping)
    virtual std::string error_code() const = 0;

    /// @brief Whether this session is bound to a protocol pipeline.
    virtual bool has_protocol_pipeline() const = 0;

    /// @brief Number of ingress messages dropped by the per-connection rate
    /// limiter since the session started. Always 0 when the limit is
    /// disabled. Exposed for ops/metrics and diagnostics.
    virtual uint64_t rate_limited_count() const = 0;

    /// @brief Enqueue a structured business message for asynchronous encode
    /// and send through the bound protocol pipeline. Encoding runs on the
    /// session strand (the same strand that owns inbound decoding), so it
    /// never races with do_receive(). Returns true once the message is
    /// accepted for encode+send; false (with @p error) only for the sync
    /// pre-flight checks: session closed, no pipeline bound, or send queue
    /// full (backpressure). Encode failures (bad route, schema mismatch) are
    /// server-side bugs -- they are logged on the strand and the message is
    /// dropped, not returned synchronously and not closing the connection.
    virtual bool send_message(const shield::transport::DecodedBody& message,
                              std::string* error = nullptr) = 0;
    virtual std::string_view protocol_codec_name() const = 0;

    /// @brief Set user data
    virtual void set_user_data(std::string key, std::string value) = 0;

    /// @brief Get user data
    virtual std::string get_user_data(std::string_view key) const = 0;

    /// @brief Snapshot of the current single-target binding.
    virtual SessionBinding binding() const = 0;

    /// @brief Install the initial (pre-auth) binding. Used once by the
    /// gateway when the connection is accepted; epoch starts from the
    /// value carried by @p initial (0 for a fresh session).
    virtual void reset_binding(SessionBinding initial) = 0;

    /// @brief Compare-and-set binding replacement. When @p expected_epoch is
    /// kAnyEpoch or equals the current epoch, the binding is replaced with
    /// the given target and player identity, epoch is incremented, and true
    /// is returned with the new binding written to @p out when non-null.
    /// On epoch mismatch the session state is left untouched and false is
    /// returned. The whole compare-write-increment runs in one critical
    /// section.
    virtual bool apply_binding(std::string target_service,
                               std::string player_id, uint32_t expected_epoch,
                               SessionBinding* out = nullptr) = 0;
};

/// @brief Session callbacks
struct SessionCallbacks {
    std::function<void(std::shared_ptr<Session>)> on_connect;
    std::function<void(std::shared_ptr<Session>, std::string_view)>
        on_disconnect;
    // Protocol path callback. DecodeLocal results are materialized before
    // dispatch. ForwardRaw results remain visible here for C++ data-plane
    // users; Drop results are skipped before this callback is invoked.
    std::function<void(std::shared_ptr<Session>,
                       const shield::transport::DispatchResult&)>
        on_packet;
    // Called once per ingress message dropped by the per-connection rate
    // limit (runs on the session's IO strand; keep it cheap).
    std::function<void()> on_rate_limited;
    std::function<std::unique_ptr<shield::transport::ProtocolPipeline>()>
        create_protocol_pipeline;
};

/// @brief TCP Session
class TcpSession : public Session,
                   public std::enable_shared_from_this<TcpSession> {
public:
    /// @param max_send_queue Max queued outgoing messages before
    ///                        backpressure kicks in (0 = unlimited).
    /// @param read_idle_timeout_ms Close the session if no data arrives for
    ///                              this many milliseconds (0 = disabled).
    /// @param rate_limit_per_second Per-connection ingress rate limit
    ///                              (0 = unlimited).
    /// @param rate_limit_burst Ingress burst allowance (0 = same as the rate).
    TcpSession(SessionId id, boost::asio::ip::tcp::socket socket,
               SessionCallbacks callbacks, size_t max_frame_size = 0,
               size_t max_send_queue = 0, uint32_t read_idle_timeout_ms = 0,
               uint32_t rate_limit_per_second = 0,
               uint32_t rate_limit_burst = 0);

    SessionId id() const override { return id_; }
    RemoteAddress remote_addr() const override { return remote_addr_; }

    bool send(const std::vector<uint8_t>& data,
              std::string* error = nullptr) override;
    bool has_protocol_pipeline() const override {
        return protocol_pipeline_ != nullptr;
    }
    uint64_t rate_limited_count() const override {
        return rate_limiter_.limited_count();
    }
    bool send_message(const shield::transport::DecodedBody& message,
                      std::string* error = nullptr) override;
    std::string_view protocol_codec_name() const override {
        if (!protocol_pipeline_) {
            return {};
        }
        return protocol_pipeline_->default_codec_name();
    }
    void close(std::string reason) override;
    bool is_alive() const override { return alive_.load(); }
    std::string error_code() const override { return error_code_; }

    void set_user_data(std::string key, std::string value) override {
        std::lock_guard<std::mutex> lock(user_data_mutex_);
        user_data_[std::move(key)] = std::move(value);
    }

    std::string get_user_data(std::string_view key) const override {
        std::lock_guard<std::mutex> lock(user_data_mutex_);
        auto it = user_data_.find(std::string(key));
        return it != user_data_.end() ? it->second : "";
    }

    SessionBinding binding() const override {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        return binding_;
    }

    void reset_binding(SessionBinding initial) override {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        binding_ = std::move(initial);
    }

    bool apply_binding(std::string target_service, std::string player_id,
                       uint32_t expected_epoch,
                       SessionBinding* out = nullptr) override {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (expected_epoch != kAnyEpoch && expected_epoch != binding_.epoch) {
            return false;
        }
        binding_.target_service = std::move(target_service);
        binding_.player_id = std::move(player_id);
        ++binding_.epoch;
        if (out != nullptr) {
            *out = binding_;
        }
        return true;
    }

    /// @brief Start receiving
    void start();

private:
    void do_receive();
    void do_async_write();
    void handle_error(std::string reason);

    SessionId id_;
    boost::asio::ip::tcp::socket socket_;
    // All per-session async work (read / write / idle timer completion) runs
    // on this strand, so TcpSession members touched only from strand handlers
    // need no further synchronization.
    boost::asio::any_io_executor strand_;
    RemoteAddress remote_addr_;
    SessionCallbacks callbacks_;
    std::atomic<bool> alive_{true};
    std::string error_code_;

    mutable std::mutex user_data_mutex_;
    std::unordered_map<std::string, std::string> user_data_;
    std::vector<uint8_t> receive_buffer_;
    std::unique_ptr<shield::transport::ProtocolPipeline> protocol_pipeline_;

    // Session binding. Guarded by binding_mutex_ because the gateway actor
    // (CAF thread) and the bridge callbacks (net threads) touch it from
    // different threads than the strand handlers.
    mutable std::mutex binding_mutex_;
    SessionBinding binding_;

    // Async send queue. send_queue_ and send_in_progress_ are only touched
    // from strand_ handlers. queued_count_ is an atomic count of reserved
    // slots (messages accepted but not yet written or rolled back); send()
    // and send_message() reserve a slot on the caller thread with fetch_add so
    // the queued total can never exceed max_send_queue_. Each write
    // completion, drop, or rollback does one fetch_sub to release its slot.
    std::deque<std::vector<uint8_t>> send_queue_;
    bool send_in_progress_ = false;
    size_t max_send_queue_ = 0;  // 0 = unlimited (queued message count)
    std::atomic<size_t> queued_count_{0};

    // Optional read idle timeout. 0 disables it.
    boost::asio::basic_waitable_timer<
        std::chrono::steady_clock,
        boost::asio::wait_traits<std::chrono::steady_clock>,
        boost::asio::any_io_executor>
        read_deadline_;
    uint32_t read_idle_timeout_ms_ = 0;

    // Per-connection ingress rate limit. Only touched from strand_ handlers
    // (the do_receive completion), so it needs no extra synchronization.
    TokenBucket rate_limiter_;
};

}  // namespace shield::net
