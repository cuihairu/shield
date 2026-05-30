// [CORE]
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "shield/protocol/protocol_handler.hpp"

namespace shield::gateway {

// Protocol-agnostic request used inside the gateway dispatch pipeline.
// HTTP, TCP, and WebSocket messages are all normalized into this shape
// before being routed to actors or Lua services.
struct GatewayRequest {
    uint64_t session_id = 0;
    std::string path;  // e.g. /api/game/action
    std::string method;  // GET/POST/TCP/WS
    std::string body;  // raw JSON payload
    std::unordered_map<std::string, std::string> headers;
    protocol::ProtocolType protocol = protocol::ProtocolType::TCP;
};

// Unified response returned from actors / middleware back to the gateway.
struct GatewayResponse {
    bool success = true;
    int status_code = 200;
    std::string body;  // JSON response body
    std::unordered_map<std::string, std::string> headers;

    static GatewayResponse ok(std::string body) {
        return {true, 200, std::move(body), {}};
    }

    static GatewayResponse error(int code, std::string message) {
        return {false, code, std::move(message), {}};
    }
};

}  // namespace shield::gateway
