#pragma once

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>

namespace shield::protocol {

// Protocol types supported by the gateway
enum class ProtocolType {
    TCP,
    HTTP,
    WEBSOCKET
};

// Base protocol handler interface
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;
    
    // Handle incoming data from a connection
    virtual void handle_data(uint64_t connection_id, const char* data, size_t length) = 0;
    
    // Handle connection establishment
    virtual void handle_connection(uint64_t connection_id) = 0;
    
    // Handle connection closure
    virtual void handle_disconnection(uint64_t connection_id) = 0;
    
    // Send data to a connection
    virtual bool send_data(uint64_t connection_id, const std::string& data) = 0;
    
    // Get protocol type
    virtual ProtocolType get_protocol_type() const = 0;
};

// HTTP request structure
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    uint64_t connection_id;
};

// HTTP response structure
struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    
    HttpResponse() {
        headers["Content-Type"] = "application/json";
        headers["Connection"] = "close";
    }
};

// WebSocket frame types
enum class WebSocketFrameType {
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A
};

// WebSocket frame structure
struct WebSocketFrame {
    WebSocketFrameType type;
    std::string payload;
    bool fin = true;
    bool masked = false;
    uint32_t mask_key = 0;
};

// Protocol handler factory
class ProtocolHandlerFactory {
public:
    using HandlerCreator = std::function<std::unique_ptr<IProtocolHandler>()>;
    
    static ProtocolHandlerFactory& instance() {
        static ProtocolHandlerFactory instance;
        return instance;
    }
    
    void register_handler(ProtocolType type, HandlerCreator creator);
    std::unique_ptr<IProtocolHandler> create_handler(ProtocolType type);
    
private:
    std::unordered_map<ProtocolType, HandlerCreator> creators_;
};

} // namespace shield::protocol