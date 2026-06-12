// [SHIELD_CORE] Message implementation
#include "shield/core/message.hpp"
#include "shield/base/error.hpp"
#include "shield/base/id.hpp"

#include <utility>

namespace shield::core {

// MessageEnvelope implementation
MessageEnvelope::MessageEnvelope(ServiceHandle sender, MethodName method,
                                 Payload payload, TraceId trace)
    : sender_(std::move(sender)),
      method_(std::move(method)),
      payload_(std::move(payload)),
      trace_(std::move(trace)) {}

// MessageResponse implementation
MessageResponse MessageResponse::ok(Payload data) {
    MessageResponse response;
    response.data_ = std::move(data);
    return response;
}

MessageResponse MessageResponse::error(Error err) {
    MessageResponse response;
    response.data_ = std::move(err);
    return response;
}

}  // namespace shield::core
