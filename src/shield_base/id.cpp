// [SHIELD_BASE] ID type implementations
#include "shield/base/id.hpp"
#include "shield/base/time.hpp"

#include <atomic>
#include <cstring>
#include <random>

namespace shield::base {

// ServiceId implementation
ServiceId ServiceId::generate() {
    static std::atomic<uint64_t> counter{1};
    return ServiceId(counter.fetch_add(1, std::memory_order_relaxed));
}

// TraceId implementation
TraceId TraceId::generate() {
    // Use high-resolution time + random for trace IDs
    static std::random_device rd;
    static std::mt19937_64 gen(rd());

    uint64_t time_part = static_cast<uint64_t>(now_ms());
    uint64_t random_part = gen();

    // Combine: 16 bits timestamp + 48 bits random
    uint64_t value = (time_part << 48) | (random_part & 0xffffffffffffULL);
    return TraceId(value);
}

TraceId TraceId::from_string(std::string_view str) {
    // Support format: "trace:0123456789abcdef" or raw hex
    if (str.size() >= 6 && str.substr(0, 6) == "trace:") {
        str = str.substr(6);
    }

    uint64_t value = 0;
    try {
        value = std::stoull(std::string(str), nullptr, 16);
    } catch (...) {
        value = 0;
    }
    return TraceId(value);
}

// NodeId implementation
NodeId NodeId::local() {
    // Generate a stable node ID based on hostname or similar
    // For now, return a placeholder
    return NodeId("node-local");
}

NodeId NodeId::from_string(std::string_view str) {
    return NodeId(str);
}

}  // namespace shield::base
