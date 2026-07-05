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

#include "shield/transport/frame.hpp"
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

/// @brief Session interface
class Session {
public:
    virtual ~Session() = default;

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
};

/// @brief Session callbacks
struct SessionCallbacks {
    std::function<void(std::shared_ptr<Session>)> on_connect;
    std::function<void(std::shared_ptr<Session>, std::string_view)>
        on_disconnect;
    std::function<void(std::shared_ptr<Session>, const std::vector<uint8_t>&)>
        on_message;
    // Protocol path callback. DecodeLocal results are materialized before
    // dispatch. ForwardRaw results remain visible here for C++ data-plane
    // users; Drop results are skipped before this callback is invoked.
    std::function<void(std::shared_ptr<Session>,
                       const shield::transport::DispatchResult&)>
        on_packet;
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
    TcpSession(SessionId id, boost::asio::ip::tcp::socket socket,
               SessionCallbacks callbacks, size_t max_frame_size = 0,
               size_t max_send_queue = 0,
               uint32_t read_idle_timeout_ms = 0);

    SessionId id() const override { return id_; }
    RemoteAddress remote_addr() const override { return remote_addr_; }

    bool send(const std::vector<uint8_t>& data,
              std::string* error = nullptr) override;
    bool has_protocol_pipeline() const override {
        return protocol_pipeline_ != nullptr;
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
    shield::transport::FrameDecoder frame_decoder_;
    std::unique_ptr<shield::transport::ProtocolPipeline> protocol_pipeline_;

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
};

}  // namespace shield::net
