// [SHIELD_BASE] Result type for error handling without exceptions
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

namespace shield::base {

// Forward declaration
class Error;

/// @brief Result type for functions that can fail without throwing exceptions
/// @tparam T The success value type
template <typename T>
class Result {
public:
    using value_type = T;
    using error_type = Error;

    // Constructors
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    // Copy/Move
    Result(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) noexcept = default;

    // Query
    bool is_ok() const { return std::holds_alternative<T>(data_); }
    bool is_error() const { return std::holds_alternative<Error>(data_); }

    // Access
    const T& value() const& { return std::get<T>(data_); }
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }

    const Error& error() const& { return std::get<Error>(data_); }
    Error& error() & { return std::get<Error>(data_); }

    // Monadic operations
    template <typename F>
    auto map(F&& f) const -> Result<decltype(f(std::declval<const T&>()))> {
        using U = decltype(f(std::declval<const T&>()));
        if (is_ok()) {
            return Result<U>(f(value()));
        }
        return Result<U>(error());
    }

    template <typename F>
    auto and_then(F&& f) const -> decltype(f(std::declval<const T&>())) {
        if (is_ok()) {
            return f(value());
        }
        return decltype(f(std::declval<const T&>()))(error());
    }

    template <typename F>
    auto or_else(F&& f) const -> Result<T> {
        if (is_error()) {
            return f(error());
        }
        return *this;
    }

private:
    std::variant<T, Error> data_;
};

// Specialization for void
template <>
class Result<void> {
public:
    Result() : ok_(true) {}
    Result(Error error) : ok_(false), error_(std::move(error)) {}

    bool is_ok() const { return ok_; }
    bool is_error() const { return !ok_; }

    const Error& error() const& { return error_; }
    Error& error() & { return error_; }

private:
    bool ok_;
    Error error_;
};

}  // namespace shield::base
