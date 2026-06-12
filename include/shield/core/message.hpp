// [SHIELD_CORE] Message types and envelope
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "shield/base/error.hpp"
#include "shield/base/id.hpp"
#include "shield/core/service_handle.hpp"

namespace shield::core {

/// @brief Payload type for message data
using Payload = std::vector<uint8_t>;
using TraceId = shield::base::TraceId;
using Error = shield::base::Error;

/// @brief Method name type
using MethodName = std::string;

/// @brief Message envelope for service-to-service communication
class MessageEnvelope {
public:
    // Constructors
    MessageEnvelope() = default;

    MessageEnvelope(ServiceHandle sender, MethodName method,
                    Payload payload, TraceId trace = TraceId{});

    // Accessors
    const ServiceHandle& sender() const { return sender_; }
    const MethodName& method() const { return method_; }
    const Payload& payload() const { return payload_; }
    const TraceId& trace() const { return trace_; }

    // Mutability for move semantics
    Payload& mutable_payload() { return payload_; }

    // Timeout for call messages (0 = no timeout)
    int32_t timeout_ms() const { return timeout_ms_; }
    void set_timeout_ms(int32_t ms) { timeout_ms_ = ms; }

    // Check if this is a call message (expects response)
    bool expects_response() const { return expects_response_; }
    void set_expects_response(bool value) { expects_response_ = value; }

private:
    ServiceHandle sender_;
    MethodName method_;
    Payload payload_;
    TraceId trace_;
    int32_t timeout_ms_ = 0;
    bool expects_response_ = false;
};

/// @brief Response from a call
class MessageResponse {
public:
    // Success response
    static MessageResponse ok(Payload data);

    // Error response
    static MessageResponse error(Error err);

    bool is_ok() const { return std::holds_alternative<Payload>(data_); }
    bool is_error() const { return std::holds_alternative<Error>(data_); }

    const Payload& payload() const { return std::get<Payload>(data_); }
    const Error& error() const { return std::get<Error>(data_); }

private:
    std::variant<Payload, Error> data_;
};

}  // namespace shield::core
