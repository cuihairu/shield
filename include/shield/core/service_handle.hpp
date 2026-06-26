// [SHIELD_CORE] Service handle - opaque reference to a service
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace shield::core {

// Forward declarations for internal CAF types
namespace detail {
class ActorHolder;
}

/// @brief Opaque handle to a service
/// Does not expose CAF types in public API
class ServiceHandle {
public:
    ServiceHandle();
    ~ServiceHandle();

    // Copy/Move
    ServiceHandle(const ServiceHandle& other);
    ServiceHandle(ServiceHandle&& other) noexcept;
    ServiceHandle& operator=(const ServiceHandle& other);
    ServiceHandle& operator=(ServiceHandle&& other) noexcept;

    // Query
    bool is_valid() const;
    bool is_local() const;
    explicit operator bool() const { return is_valid(); }

    // Name associated with this handle (if any)
    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // String representation
    std::string to_string() const;

    // Internal accessor for CAF bridge layer
    detail::ActorHolder* internal() const { return holder_; }
    void internal_set(detail::ActorHolder* holder);

private:
    detail::ActorHolder* holder_ = nullptr;
    std::string name_;
    bool is_local_ = true;

    // Private constructor for internal use
    explicit ServiceHandle(detail::ActorHolder* holder, std::string name = "",
                           bool is_local = true);

    friend class ServiceRegistry;
    friend class CafAdapter;
};

/// @brief Opaque handle to a timer
class TimerHandle {
public:
    TimerHandle();
    ~TimerHandle();

    TimerHandle(const TimerHandle& other);
    TimerHandle(TimerHandle&& other) noexcept;
    TimerHandle& operator=(const TimerHandle& other);
    TimerHandle& operator=(TimerHandle&& other) noexcept;

    bool is_valid() const;
    explicit operator bool() const { return is_valid(); }

    void cancel();

private:
    uint64_t id_;
    friend class CafAdapter;
};

/// @brief Opaque handle to a forked task
class TaskHandle {
public:
    TaskHandle();
    ~TaskHandle();

    TaskHandle(const TaskHandle& other);
    TaskHandle(TaskHandle&& other) noexcept;
    TaskHandle& operator=(const TaskHandle& other);
    TaskHandle& operator=(TaskHandle&& other) noexcept;

    bool is_valid() const;
    explicit operator bool() const { return is_valid(); }

    void cancel();

private:
    uint64_t id_;
    friend class CafAdapter;
};

}  // namespace shield::core
