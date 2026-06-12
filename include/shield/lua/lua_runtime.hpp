// [SHIELD_LUA] Lua Runtime
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>
#include <sol/sol.hpp>

namespace shield::lua {

// Opaque handle to a Lua VM state
class LuaVM;
class LuaServiceManager;

/// @brief Coroutine scheduler for managing suspended coroutines
class CoroutineScheduler {
public:
    /// @brief Unique ID for a suspended coroutine
    using CoroutineId = uint64_t;

    /// @brief Status of a suspended coroutine
    enum class Status {
        Pending,    // Waiting for response/timer
        Ready,      // Ready to resume
        Completed,  // Completed successfully
        Failed,     // Failed with error
    };

    /// @brief A suspended coroutine context
    struct SuspendedCoroutine {
        CoroutineId id;
        std::string service_id;
        sol::coroutine coroutine;
        sol::state_view state;
        int64_t deadline_ms;
        Status status = Status::Pending;
        nlohmann::json result;  // For call responses
        std::string error;      // For errors
    };

    explicit CoroutineScheduler();

    // Suspend current coroutine with a deadline
    /// @param service_id Owner service ID
    /// @param co The coroutine to suspend
    /// @param timeout_ms Timeout in milliseconds
    /// @return Coroutine ID for resuming
    CoroutineId suspend(const std::string& service_id,
                       sol::coroutine co,
                       int32_t timeout_ms);

    // Resume a coroutine with result
    /// @param id Coroutine ID
    /// @param result Response result (as JSON array)
    /// @return true if coroutine was resumed
    bool resume(CoroutineId id, const nlohmann::json& result);

    // Resume a coroutine with error
    /// @param id Coroutine ID
    /// @param error Error message
    /// @return true if coroutine was resumed
    bool resume_with_error(CoroutineId id, const std::string& error);

    // Cancel a coroutine (e.g., on service exit)
    /// @param id Coroutine ID
    /// @return true if coroutine was cancelled
    bool cancel(CoroutineId id);

    // Cancel all coroutines for a service
    void cancel_all_for_service(const std::string& service_id);

    // Check for timed out coroutines
    /// @param now_ms Current monotonic time in milliseconds
    /// @return Number of coroutines timed out
    int check_timeouts(int64_t now_ms);

    // Get active coroutine count
    size_t active_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Opaque ServiceHandle userdata for Lua
/// This prevents direct string manipulation of service IDs
class ServiceHandle {
public:
    explicit ServiceHandle(std::string service_id)
        : service_id_(std::move(service_id)) {}

    const std::string& id() const { return service_id_; }

    // Check if handle is valid
    bool valid() const { return !service_id_.empty(); }

    // Lua userdata operations
    static void register_usertype(sol::state& lua);

private:
    std::string service_id_;
};

/// @brief Timer manager for scheduling and canceling timers
class TimerManager {
public:
    using TimerId = uint64_t;

    enum class TimerType {
        Once,   // One-shot timer
        FixedDelay,  // Repeating timer with fixed delay
    };

    struct TimerCallback {
        sol::function callback;
        sol::state_view state;
        std::string service_id;
    };

    explicit TimerManager();

    // Schedule a one-shot timer
    /// @param delay_ms Delay in milliseconds
    /// @param callback Function to call when timer fires
    /// @param service_id Owner service ID
    /// @return Timer ID for cancellation
    TimerId schedule_once(int64_t delay_ms,
                          sol::function callback,
                          const std::string& service_id);

    // Schedule a repeating timer
    /// @param interval_ms Interval in milliseconds
    /// @param callback Function to call when timer fires
    /// @param service_id Owner service ID
    /// @return Timer ID for cancellation
    TimerId schedule_fixed_delay(int64_t interval_ms,
                                sol::function callback,
                                const std::string& service_id);

    // Cancel a timer
    /// @param id Timer ID
    /// @return true if timer was cancelled
    bool cancel(TimerId id);

    // Cancel all timers for a service
    void cancel_all_for_service(const std::string& service_id);

    // Check and fire expired timers
    /// @param now_ms Current monotonic time in milliseconds
    /// @return Number of timers fired
    int check_and_fire(int64_t now_ms);

    // Get active timer count
    size_t active_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Lua runtime manager
/// Manages a pool of Lua VMs and provides API registration
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    // Non-copyable
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Create a new Lua VM
    std::shared_ptr<LuaVM> create_vm();

    // Load a script into a VM
    /// @param vm The VM to load into
    /// @param script_path Path to the Lua script
    /// @return true if successful
    bool load_script(std::shared_ptr<LuaVM> vm, std::string_view script_path);

    // Load a Lua service module that returns a table.
    bool load_service_module(std::shared_ptr<LuaVM> vm,
                             std::string_view script_path,
                             std::string* error = nullptr);

    // Call a function on the loaded service module table.
    bool call_service_function(std::shared_ptr<LuaVM> vm,
                               std::string_view func_name,
                               const nlohmann::json& args,
                               std::string* error = nullptr);

    // Dispatch a service method with JSON-array arguments and collect all
    // return values as a JSON array. Missing or non-function methods are errors.
    bool call_service_method(std::shared_ptr<LuaVM> vm,
                             std::string_view method_name,
                             const nlohmann::json& args,
                             nlohmann::json* returns = nullptr,
                             std::string* error = nullptr);

    // Call a function in a VM
    /// @param vm The VM to call in
    /// @param func_name Function name
    /// @param args Arguments (as JSON string)
    /// @return Result as JSON string
    std::string call_function(std::shared_ptr<LuaVM> vm,
                             std::string_view func_name,
                             std::string_view args = "{}");

    // Register API functions
    void register_api(std::shared_ptr<LuaVM> vm);

    // Bind service-management APIs for VMs created by this runtime.
    void set_service_manager(LuaServiceManager* manager);

    // Get coroutine scheduler for this runtime
    CoroutineScheduler& coroutine_scheduler() { return *coroutine_scheduler_; }

    // Get timer manager for this runtime
    TimerManager& timer_manager() { return *timer_manager_; }

    // Get global variable
    std::string get_global(std::shared_ptr<LuaVM> vm,
                          std::string_view name);

    // Set global variable
    void set_global(std::shared_ptr<LuaVM> vm,
                   std::string_view name,
                   std::string_view value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Coroutine scheduler (shared across all VMs in this runtime)
    std::unique_ptr<CoroutineScheduler> coroutine_scheduler_;

    // Timer manager (shared across all VMs in this runtime)
    std::unique_ptr<TimerManager> timer_manager_;
};

/// @brief Per-VM context for a Lua service
class ServiceContext {
public:
    std::string service_id;
    std::string service_name;
    std::string module_path;

    // Current message context (only valid during handler execution)
    std::string sender_id;
    std::string trace_id;
    int64_t deadline_ms = 0;
};

}  // namespace shield::lua
