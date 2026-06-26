// [SHIELD_BASE] Error type for structured error information
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace shield::base {

/// @brief Structured error type
class Error {
public:
    Error() = default;

    Error(std::string code, std::string message, bool retryable = false,
          std::optional<std::string> detail = std::nullopt)
        : code_(std::move(code)),
          message_(std::move(message)),
          retryable_(retryable),
          detail_(detail ? *detail : "") {}

    // Accessors
    const std::string& code() const { return code_; }
    const std::string& message() const { return message_; }
    bool retryable() const { return retryable_; }
    const std::string& detail() const { return detail_; }

    // Fluent builder pattern
    Error& with_code(std::string code) {
        code_ = std::move(code);
        return *this;
    }

    Error& with_message(std::string message) {
        message_ = std::move(message);
        return *this;
    }

    Error& with_retryable(bool retryable = true) {
        retryable_ = retryable;
        return *this;
    }

    Error& with_detail(std::string detail) {
        detail_ = std::move(detail);
        return *this;
    }

    Error& with_context(std::string key, std::string value) {
        context_[std::move(key)] = std::move(value);
        return *this;
    }

    const std::unordered_map<std::string, std::string>& context() const {
        return context_;
    }

    // Valid check
    explicit operator bool() const { return !code_.empty(); }

private:
    std::string code_;
    std::string message_;
    bool retryable_ = false;
    std::string detail_;
    std::unordered_map<std::string, std::string> context_;
};

// Common error codes
namespace errors {
constexpr const char* TIMEOUT = "timeout";
constexpr const char* NOT_FOUND = "not_found";
constexpr const char* ALREADY_EXISTS = "already_exists";
constexpr const char* INVALID_ARGUMENT = "invalid_argument";
constexpr const char* PERMISSION_DENIED = "permission_denied";
constexpr const char* UNAVAILABLE = "unavailable";
constexpr const char* INTERNAL = "internal";
constexpr const char* SERVICE_NOT_FOUND = "service_not_found";
constexpr const char* SERVICE_EXITING = "service_exiting";
constexpr const char* MAILBOX_FULL = "mailbox_full";
constexpr const char* MESSAGE_TOO_LARGE = "message_too_large";
constexpr const char* SERIALIZATION_FAILED = "serialization_failed";
constexpr const char* LUA_SCRIPT_ERROR = "lua_script_error";
constexpr const char* LUA_TIMEOUT = "lua_timeout";
constexpr const char* MODULE_UNAVAILABLE = "module_unavailable";
}  // namespace errors

}  // namespace shield::base
