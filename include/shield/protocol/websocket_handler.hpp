#pragma once

#include <functional>
#include <random>
#include <unordered_map>

#include "shield/net/session.hpp"
#include "shield/protocol/protocol_handler.hpp"

namespace shield::protocol {

// WebSocket connection states
enum class WebSocketState { CONNECTING, OPEN, CLOSING, CLOSED };

// WebSocket connection info
struct WebSocketConnection {
    uint64_t connection_id;
    WebSocketState state;
    std::string buffer;
    bool is_server = true;
};

// WebSocket protocol handler implementation
class WebSocketProtocolHandler : public IProtocolHandler {
public:
    using MessageHandler =
        std::function<void(uint64_t connection_id, const std::string &message)>;

    WebSocketProtocolHandler();
    ~WebSocketProtocolHandler() override = default;

    void handle_data(uint64_t connection_id, const char *data,
                     size_t length) override;
    void handle_connection(uint64_t connection_id) override;
    void handle_disconnection(uint64_t connection_id) override;
    bool send_data(uint64_t connection_id, const std::string &data) override;
    ProtocolType get_protocol_type() const override {
        return ProtocolType::WEBSOCKET;
    }

    // WebSocket-specific methods
    void set_session_provider(
        std::function<std::shared_ptr<net::Session>(uint64_t)> provider);
    void set_message_handler(MessageHandler handler);
    bool send_text_frame(uint64_t connection_id, const std::string &text);
    bool send_binary_frame(uint64_t connection_id, const std::string &data);
    bool send_ping(uint64_t connection_id, const std::string &payload = "");
    bool send_pong(uint64_t connection_id, const std::string &payload = "");
    bool close_connection(uint64_t connection_id, uint16_t code = 1000,
                          const std::string &reason = "");

private:
    bool handle_handshake(uint64_t connection_id, const std::string &request);
    std::string generate_websocket_accept(const std::string &key);
    WebSocketFrame parse_frame(const std::string &data, size_t &bytes_consumed);
    std::string encode_frame(const WebSocketFrame &frame);
    void handle_frame(uint64_t connection_id, const WebSocketFrame &frame);
    uint32_t generate_mask_key();

    std::unordered_map<uint64_t, WebSocketConnection> connections_;
    std::function<std::shared_ptr<net::Session>(uint64_t)> session_provider_;
    MessageHandler message_handler_;
    std::random_device random_device_;
    std::mt19937 random_generator_;
};

// Factory function
std::unique_ptr<WebSocketProtocolHandler> create_websocket_handler();

}  // namespace shield::protocol