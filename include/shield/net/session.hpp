// [SHIELD_NET] Session types
#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <atomic>
#include <cstdint>
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

namespace shield::net {

/// @brief Session ID
using SessionId = uint64_t;

/// @brief Remote address
struct RemoteAddress {
    std::string ip;
    uint16_t port;

    std::string to_string() const {
        return ip + ":" + std::to_string(port);
    }
};

/// @brief Session close reasons
namespace CloseReason {
    constexpr const char* NORMAL = "normal";
    constexpr const char* TIMEOUT = "timeout";
    constexpr const char* ERROR = "error";
    constexpr const char* KICKED = "kicked";
    constexpr const char* SHUTDOWN = "shutdown";
}

/// @brief Session interface
class Session {
public:
    virtual ~Session() = default;

    /// @brief Get session ID
    virtual SessionId id() const = 0;

    /// @brief Get remote address
    virtual RemoteAddress remote_addr() const = 0;

    /// @brief Send data to client
    virtual bool send(const std::vector<uint8_t>& data) = 0;

    /// @brief Close session
    virtual void close(std::string reason) = 0;

    /// @brief Check if session is alive
    virtual bool is_alive() const = 0;

    /// @brief Set user data
    virtual void set_user_data(std::string key, std::string value) = 0;

    /// @brief Get user data
    virtual std::string get_user_data(std::string_view key) const = 0;
};

/// @brief Session callbacks
struct SessionCallbacks {
    std::function<void(std::shared_ptr<Session>)> on_connect;
    std::function<void(std::shared_ptr<Session>, std::string_view)> on_disconnect;
    std::function<void(std::shared_ptr<Session>, const std::vector<uint8_t>&)> on_message;
};

/// @brief TCP Session
class TcpSession : public Session, public std::enable_shared_from_this<TcpSession> {
public:
    TcpSession(SessionId id, boost::asio::ip::tcp::socket socket,
              SessionCallbacks callbacks);

    SessionId id() const override { return id_; }
    RemoteAddress remote_addr() const override { return remote_addr_; }

    bool send(const std::vector<uint8_t>& data) override;
    void close(std::string reason) override;
    bool is_alive() const override { return alive_; }

    void set_user_data(std::string key, std::string value) override {
        user_data_[std::move(key)] = std::move(value);
    }

    std::string get_user_data(std::string_view key) const override {
        auto it = user_data_.find(std::string(key));
        return it != user_data_.end() ? it->second : "";
    }

    /// @brief Start receiving
    void start();

private:
    void do_receive();
    void handle_error(std::string reason);

    SessionId id_;
    boost::asio::ip::tcp::socket socket_;
    RemoteAddress remote_addr_;
    SessionCallbacks callbacks_;
    bool alive_ = true;

    std::unordered_map<std::string, std::string> user_data_;
    std::vector<uint8_t> receive_buffer_;
    shield::transport::FrameDecoder frame_decoder_;
};

}  // namespace shield::net
