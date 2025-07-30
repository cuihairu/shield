#include "shield/protocol/websocket_handler.hpp"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <regex>
#include <sstream>

#include "shield/log/logger.hpp"
#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#include <machine/endian.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#else
#include <endian.h>
#endif

namespace shield::protocol {

namespace {
const std::string WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string base64_encode(const unsigned char* data, size_t length) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    BIO_write(bio, data, length);
    BIO_flush(bio);

    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);

    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);
    return result;
}
}  // namespace

WebSocketProtocolHandler::WebSocketProtocolHandler()
    : random_generator_(random_device_()) {}

void WebSocketProtocolHandler::handle_data(uint64_t connection_id,
                                           const char* data, size_t length) {
    auto it = connections_.find(connection_id);
    if (it == connections_.end()) {
        return;
    }

    auto& conn = it->second;
    conn.buffer.append(data, length);

    if (conn.state == WebSocketState::CONNECTING) {
        // Check if handshake is complete
        if (conn.buffer.find("\r\n\r\n") != std::string::npos) {
            if (handle_handshake(connection_id, conn.buffer)) {
                conn.state = WebSocketState::OPEN;
                SHIELD_LOG_DEBUG
                    << "WebSocket handshake completed for connection "
                    << connection_id;
            } else {
                SHIELD_LOG_ERROR << "WebSocket handshake failed for connection "
                                 << connection_id;
                connections_.erase(connection_id);
                return;
            }
            conn.buffer.clear();
        }
    } else if (conn.state == WebSocketState::OPEN) {
        // Parse WebSocket frames
        while (!conn.buffer.empty()) {
            size_t bytes_consumed = 0;
            try {
                WebSocketFrame frame = parse_frame(conn.buffer, bytes_consumed);
                if (bytes_consumed > 0) {
                    handle_frame(connection_id, frame);
                    conn.buffer.erase(0, bytes_consumed);
                } else {
                    // Not enough data for a complete frame
                    break;
                }
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "WebSocket frame parsing error: "
                                 << e.what();
                close_connection(connection_id, 1002, "Protocol error");
                break;
            }
        }
    }
}

void WebSocketProtocolHandler::handle_connection(uint64_t connection_id) {
    SHIELD_LOG_DEBUG << "WebSocket connection established: " << connection_id;
    WebSocketConnection conn;
    conn.connection_id = connection_id;
    conn.state = WebSocketState::CONNECTING;
    connections_[connection_id] = std::move(conn);
}

void WebSocketProtocolHandler::handle_disconnection(uint64_t connection_id) {
    SHIELD_LOG_DEBUG << "WebSocket connection closed: " << connection_id;
    connections_.erase(connection_id);
}

bool WebSocketProtocolHandler::send_data(uint64_t connection_id,
                                         const std::string& data) {
    return send_text_frame(connection_id, data);
}

void WebSocketProtocolHandler::set_session_provider(
    std::function<std::shared_ptr<net::Session>(uint64_t)> provider) {
    session_provider_ = std::move(provider);
}

void WebSocketProtocolHandler::set_message_handler(MessageHandler handler) {
    message_handler_ = std::move(handler);
}

bool WebSocketProtocolHandler::send_text_frame(uint64_t connection_id,
                                               const std::string& text) {
    WebSocketFrame frame;
    frame.type = WebSocketFrameType::TEXT;
    frame.payload = text;
    frame.fin = true;
    frame.masked = false;

    std::string encoded = encode_frame(frame);

    if (session_provider_) {
        auto session = session_provider_(connection_id);
        if (session) {
            session->send(encoded.c_str(), encoded.size());
            return true;
        }
    }
    return false;
}

bool WebSocketProtocolHandler::send_binary_frame(uint64_t connection_id,
                                                 const std::string& data) {
    WebSocketFrame frame;
    frame.type = WebSocketFrameType::BINARY;
    frame.payload = data;
    frame.fin = true;
    frame.masked = false;

    std::string encoded = encode_frame(frame);

    if (session_provider_) {
        auto session = session_provider_(connection_id);
        if (session) {
            session->send(encoded.c_str(), encoded.size());
            return true;
        }
    }
    return false;
}

bool WebSocketProtocolHandler::send_ping(uint64_t connection_id,
                                         const std::string& payload) {
    WebSocketFrame frame;
    frame.type = WebSocketFrameType::PING;
    frame.payload = payload;
    frame.fin = true;
    frame.masked = false;

    std::string encoded = encode_frame(frame);

    if (session_provider_) {
        auto session = session_provider_(connection_id);
        if (session) {
            session->send(encoded.c_str(), encoded.size());
            return true;
        }
    }
    return false;
}

bool WebSocketProtocolHandler::send_pong(uint64_t connection_id,
                                         const std::string& payload) {
    WebSocketFrame frame;
    frame.type = WebSocketFrameType::PONG;
    frame.payload = payload;
    frame.fin = true;
    frame.masked = false;

    std::string encoded = encode_frame(frame);

    if (session_provider_) {
        auto session = session_provider_(connection_id);
        if (session) {
            session->send(encoded.c_str(), encoded.size());
            return true;
        }
    }
    return false;
}

bool WebSocketProtocolHandler::close_connection(uint64_t connection_id,
                                                uint16_t code,
                                                const std::string& reason) {
    auto it = connections_.find(connection_id);
    if (it != connections_.end()) {
        it->second.state = WebSocketState::CLOSING;
    }

    WebSocketFrame frame;
    frame.type = WebSocketFrameType::CLOSE;
    frame.fin = true;
    frame.masked = false;

    // Encode close code and reason
    std::string payload;
    uint16_t network_code = htobe16(code);
    payload.append(reinterpret_cast<const char*>(&network_code), 2);
    payload.append(reason);
    frame.payload = payload;

    std::string encoded = encode_frame(frame);

    if (session_provider_) {
        auto session = session_provider_(connection_id);
        if (session) {
            session->send(encoded.c_str(), encoded.size());
            return true;
        }
    }
    return false;
}

bool WebSocketProtocolHandler::handle_handshake(uint64_t connection_id,
                                                const std::string& request) {
    std::istringstream stream(request);
    std::string line;
    std::unordered_map<std::string, std::string> headers;

    // Parse request line
    std::getline(stream, line);
    if (line.find("GET") != 0 || line.find("HTTP/1.1") == std::string::npos) {
        return false;
    }

    // Parse headers
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        auto colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Trim whitespace and convert to lowercase for key
            key.erase(key.find_last_not_of(" \t") + 1);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            headers[key] = value;
        }
    }

    // Validate WebSocket headers
    if (headers["upgrade"] != "websocket" ||
        headers["connection"].find("Upgrade") == std::string::npos ||
        headers["sec-websocket-version"] != "13") {
        return false;
    }

    std::string websocket_key = headers["sec-websocket-key"];
    if (websocket_key.empty()) {
        return false;
    }

    // Generate accept key
    std::string accept_key = generate_websocket_accept(websocket_key);

    // Send handshake response
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n";
    response << "Upgrade: websocket\r\n";
    response << "Connection: Upgrade\r\n";
    response << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
    response << "\r\n";

    if (session_provider_) {
        auto session = session_provider_(connection_id);
        if (session) {
            std::string response_str = response.str();
            session->send(response_str.c_str(), response_str.size());
            return true;
        }
    }

    return false;
}

std::string WebSocketProtocolHandler::generate_websocket_accept(
    const std::string& key) {
    std::string combined = key + WEBSOCKET_GUID;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()),
         combined.length(), hash);

    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

WebSocketFrame WebSocketProtocolHandler::parse_frame(const std::string& data,
                                                     size_t& bytes_consumed) {
    bytes_consumed = 0;

    if (data.size() < 2) {
        throw std::runtime_error(
            "Insufficient data for WebSocket frame header");
    }

    WebSocketFrame frame;

    uint8_t first_byte = static_cast<uint8_t>(data[0]);
    uint8_t second_byte = static_cast<uint8_t>(data[1]);

    frame.fin = (first_byte & 0x80) != 0;
    uint8_t opcode = first_byte & 0x0F;
    frame.type = static_cast<WebSocketFrameType>(opcode);

    frame.masked = (second_byte & 0x80) != 0;
    uint64_t payload_length = second_byte & 0x7F;

    size_t header_size = 2;

    // Extended payload length
    if (payload_length == 126) {
        if (data.size() < header_size + 2) {
            throw std::runtime_error(
                "Insufficient data for extended payload length");
        }
        payload_length = (static_cast<uint16_t>(data[header_size]) << 8) |
                         static_cast<uint16_t>(data[header_size + 1]);
        header_size += 2;
    } else if (payload_length == 127) {
        if (data.size() < header_size + 8) {
            throw std::runtime_error(
                "Insufficient data for extended payload length");
        }
        payload_length = 0;
        for (int i = 0; i < 8; ++i) {
            payload_length = (payload_length << 8) |
                             static_cast<uint8_t>(data[header_size + i]);
        }
        header_size += 8;
    }

    // Masking key
    if (frame.masked) {
        if (data.size() < header_size + 4) {
            throw std::runtime_error("Insufficient data for masking key");
        }
        frame.mask_key = (static_cast<uint32_t>(data[header_size]) << 24) |
                         (static_cast<uint32_t>(data[header_size + 1]) << 16) |
                         (static_cast<uint32_t>(data[header_size + 2]) << 8) |
                         static_cast<uint32_t>(data[header_size + 3]);
        header_size += 4;
    }

    // Payload
    if (data.size() < header_size + payload_length) {
        throw std::runtime_error("Insufficient data for payload");
    }

    frame.payload = data.substr(header_size, payload_length);

    // Unmask payload if needed
    if (frame.masked) {
        for (size_t i = 0; i < frame.payload.size(); ++i) {
            uint8_t mask_byte = (frame.mask_key >> (8 * (3 - (i % 4)))) & 0xFF;
            frame.payload[i] ^= mask_byte;
        }
    }

    bytes_consumed = header_size + payload_length;
    return frame;
}

std::string WebSocketProtocolHandler::encode_frame(
    const WebSocketFrame& frame) {
    std::string result;

    // First byte: FIN + RSV + Opcode
    uint8_t first_byte = static_cast<uint8_t>(frame.type);
    if (frame.fin) {
        first_byte |= 0x80;
    }
    result.push_back(first_byte);

    // Second byte: MASK + Payload length
    uint64_t payload_length = frame.payload.size();
    uint8_t second_byte = 0;

    if (frame.masked) {
        second_byte |= 0x80;
    }

    if (payload_length < 126) {
        second_byte |= static_cast<uint8_t>(payload_length);
        result.push_back(second_byte);
    } else if (payload_length <= 0xFFFF) {
        second_byte |= 126;
        result.push_back(second_byte);
        result.push_back(static_cast<uint8_t>((payload_length >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(payload_length & 0xFF));
    } else {
        second_byte |= 127;
        result.push_back(second_byte);
        for (int i = 7; i >= 0; --i) {
            result.push_back(
                static_cast<uint8_t>((payload_length >> (8 * i)) & 0xFF));
        }
    }

    // Masking key
    if (frame.masked) {
        result.push_back(static_cast<uint8_t>((frame.mask_key >> 24) & 0xFF));
        result.push_back(static_cast<uint8_t>((frame.mask_key >> 16) & 0xFF));
        result.push_back(static_cast<uint8_t>((frame.mask_key >> 8) & 0xFF));
        result.push_back(static_cast<uint8_t>(frame.mask_key & 0xFF));
    }

    // Payload
    std::string payload = frame.payload;
    if (frame.masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            uint8_t mask_byte = (frame.mask_key >> (8 * (3 - (i % 4)))) & 0xFF;
            payload[i] ^= mask_byte;
        }
    }
    result.append(payload);

    return result;
}

void WebSocketProtocolHandler::handle_frame(uint64_t connection_id,
                                            const WebSocketFrame& frame) {
    switch (frame.type) {
        case WebSocketFrameType::TEXT:
        case WebSocketFrameType::BINARY:
            if (message_handler_) {
                message_handler_(connection_id, frame.payload);
            }
            break;

        case WebSocketFrameType::PING:
            send_pong(connection_id, frame.payload);
            break;

        case WebSocketFrameType::PONG:
            // Handle pong (could update ping/pong timing)
            SHIELD_LOG_DEBUG << "Received WebSocket pong from connection "
                             << connection_id;
            break;

        case WebSocketFrameType::CLOSE: {
            auto it = connections_.find(connection_id);
            if (it != connections_.end()) {
                it->second.state = WebSocketState::CLOSED;
            }

            uint16_t close_code = 1000;
            std::string close_reason;

            if (frame.payload.size() >= 2) {
                close_code = be16toh(
                    *reinterpret_cast<const uint16_t*>(frame.payload.data()));
                if (frame.payload.size() > 2) {
                    close_reason = frame.payload.substr(2);
                }
            }

            SHIELD_LOG_DEBUG
                << "WebSocket close frame received from connection "
                << connection_id << ", code: " << close_code;

            // Echo close frame back
            close_connection(connection_id, close_code, close_reason);
            break;
        }

        default:
            SHIELD_LOG_WARN << "Unknown WebSocket frame type: "
                            << static_cast<int>(frame.type);
            break;
    }
}

uint32_t WebSocketProtocolHandler::generate_mask_key() {
    return random_generator_();
}

// Factory function
std::unique_ptr<WebSocketProtocolHandler> create_websocket_handler() {
    return std::make_unique<WebSocketProtocolHandler>();
}

}  // namespace shield::protocol