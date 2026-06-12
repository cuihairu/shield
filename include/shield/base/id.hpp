// [SHIELD_BASE] ID types for Shield runtime
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace shield::base {

/// @brief Service identifier (unique within a runtime)
class ServiceId {
public:
    ServiceId() : value_(0) {}
    explicit ServiceId(uint64_t value) : value_(value) {}

    uint64_t value() const { return value_; }

    bool is_valid() const { return value_ != 0; }
    explicit operator bool() const { return is_valid(); }

    std::string to_string() const {
        return "svc:" + std::to_string(value_);
    }

    static ServiceId generate();

    bool operator==(const ServiceId& other) const {
        return value_ == other.value_;
    }

    bool operator!=(const ServiceId& other) const {
        return value_ != other.value_;
    }

private:
    uint64_t value_;
};

/// @brief Trace identifier for request tracking
class TraceId {
public:
    TraceId() : value_(0) {}
    explicit TraceId(uint64_t value) : value_(value) {}

    uint64_t value() const { return value_; }

    bool is_valid() const { return value_ != 0; }
    explicit operator bool() const { return is_valid(); }

    std::string to_string() const {
        if (value_ == 0) return "trace:none";
        // Format as 16-character hex
        char buf[32];
        snprintf(buf, sizeof(buf), "trace:%016llx",
                 static_cast<unsigned long long>(value_));
        return std::string(buf);
    }

    static TraceId generate();
    static TraceId from_string(std::string_view str);

    bool operator==(const TraceId& other) const {
        return value_ == other.value_;
    }

    bool operator!=(const TraceId& other) const {
        return value_ != other.value_;
    }

private:
    uint64_t value_;
};

/// @brief Node identifier for cluster communication
class NodeId {
public:
    static constexpr size_t MAX_LENGTH = 64;

    NodeId() = default;
    explicit NodeId(std::string_view value) : value_(value) {}

    const std::string& value() const { return value_; }

    bool is_valid() const { return !value_.empty(); }
    explicit operator bool() const { return is_valid(); }

    std::string to_string() const { return value_; }

    static NodeId local();
    static NodeId from_string(std::string_view str);

    bool operator==(const NodeId& other) const {
        return value_ == other.value_;
    }

    bool operator!=(const NodeId& other) const {
        return value_ != other.value_;
    }

private:
    std::string value_;
};

}  // namespace shield::base
