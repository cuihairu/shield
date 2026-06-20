// [SHIELD_LUA] Lua Runtime
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
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
        std::function<void()> callback;
        sol::function raw_callback;   // original Lua function, for coroutine wrapping
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

    // Schedule a one-shot timer with a native (C++) callback. Used by runtime
    // internals such as coroutine sleep that need to resume a suspended Lua
    // coroutine without going through a sol::function.
    TimerId schedule_once_fn(int64_t delay_ms,
                             std::function<void()> callback,
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

    // Check and fire expired timers with error reporting.
    /// @param now_ms Current monotonic time in milliseconds
    /// @param on_error Callback invoked with (service_id, error_message) when a timer throws
    /// @return Number of timers fired
    int check_and_fire(int64_t now_ms,
                       std::function<void(const std::string& service_id,
                                          const std::string& error)> on_error);

    // Visitor-based fire: calls `visitor(service_id, raw_callback)` for each
    // expired timer instead of invoking the callback directly.  The visitor
    // can wrap the callback in a coroutine, run it synchronously, etc.
    // Repeating timers are rescheduled after the visitor returns.
    /// @param now_ms Current monotonic time in milliseconds
    /// @param visitor Called with (service_id, raw sol::function) for each expired timer
    /// @return Number of timers processed
    int check_and_fire_each(int64_t now_ms,
                            std::function<void(const std::string& service_id,
                                               sol::function callback)> visitor);

    // Get active timer count
    size_t active_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Core repeating-timer scheduler taking a native callback. Used by the
    // public sol::function overload and by internal rescheduling.
    TimerId schedule_fixed_delay_fn(int64_t interval_ms,
                                    std::function<void()> callback,
                                    const std::string& service_id);
};

/// @brief Mailbox for service message queuing
class Mailbox {
public:
    /// @brief Message priority
    enum class Priority {
        Low = 3,
        Normal = 2,
        High = 1,
        Urgent = 0,
    };

    /// @brief Backpressure strategy when mailbox is full
    enum class Backpressure {
        DropNewest,  // Drop new message (default)
        DropOldest,  // Drop oldest message
        Block,       // Block producer
    };

    /// @brief Queued message
    struct Message {
        std::string sender;
        std::string method;
        nlohmann::json args;
        Priority priority = Priority::Normal;
        int64_t timestamp_ms = 0;
        // Coroutine call correlation. Defaults describe a plain send.
        // Call-request: call_session != 0, call_reply_to = caller service id.
        // Call-response: is_call_response = true, call_session matches the
        // request, args = JSON array [ok, values...].
        uint64_t call_session = 0;
        std::string call_reply_to;
        bool is_call_response = false;
    };

    explicit Mailbox(size_t max_size = 1000);
    ~Mailbox();

    // Push a message to the mailbox
    /// @param msg Message to push
    /// @param strategy Backpressure strategy
    /// @return true if message was queued, false if dropped
    bool push(const Message& msg, Backpressure strategy = Backpressure::DropNewest);

    // Pop next message (highest priority first, then FIFO within same priority)
    /// @param out Output message
    /// @return true if message was popped
    bool pop(Message* out);

    // Get current size
    size_t size() const;

    // Check if mailbox is full
    bool full() const;

    // Clear all messages
    void clear();

    // Get dropped message count
    size_t dropped_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief LuaPack binary encoder for message serialization
class LuaPackEncoder {
public:
    // Type tags as per LuaPack specification
    enum class TypeTag : uint8_t {
        Nil = 0x00,
        False = 0x01,
        True = 0x02,
        Integer = 0x03,
        Number = 0x04,
        ShortString = 0x05,
        String = 0x06,
        Array = 0x07,
        Map = 0x08,
        ServiceHandle = 0x10,
        Extension = 0xFF,
    };

    static constexpr uint8_t VERSION = 1;
    static constexpr uint8_t MAGIC_HIGH = 0x4C;  // 'L'
    static constexpr uint8_t MAGIC_LOW = 0x50;   // 'P'

    // Configuration
    struct Config {
        size_t max_nesting_depth = 64;
        size_t max_string_length = 1048576;  // 1MB
        size_t max_array_length = 1000000;
        size_t max_map_entries = 100000;
    };

    explicit LuaPackEncoder(const Config& config);

    // Encode a Lua value to LuaPack format
    /// @param lua Lua state
    /// @param value Value to encode
    /// @param out_bytes Output buffer
    /// @return true if encoding succeeded
    bool encode(sol::state_view lua, const sol::object& value,
               std::vector<uint8_t>& out_bytes);

    // Get last error message
    std::string error() const { return error_; }

private:
    bool encode_value(sol::state_view lua, const sol::object& value,
                     std::vector<uint8_t>& out, size_t depth);

    Config config_;
    std::string error_;
};

/// @brief LuaPack binary decoder for message deserialization
class LuaPackDecoder {
public:
    explicit LuaPackDecoder();

    // Decode LuaPack bytes to Lua value
    /// @param lua Lua state
    /// @param bytes Input bytes
    /// @param out_bytes_consumed Number of bytes consumed
    /// @return Decoded Lua value or nil on error
    sol::object decode(sol::state_view lua, const std::vector<uint8_t>& bytes,
                       size_t& out_bytes_consumed);

    // Get last error message
    std::string error() const { return error_; }

private:
    sol::object decode_value(sol::state_view lua, const uint8_t* data,
                              size_t size, size_t& out_consumed);

    std::string error_;
};

/// @brief Lua script cache configuration
struct LuaCacheConfig {
    bool enabled = true;
    size_t max_size = 100;
    int64_t ttl_seconds = 0;  // 0 = never expire
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

    // Invoke a service method inside a Lua coroutine so it may yield via
    // shield.sleep / (future) coroutine-aware call. Used by the async message
    // dispatch path (process_mailbox). If the handler completes without
    // yielding it behaves like call_service_method; if it yields, this returns
    // true (no error) and the suspended coroutine is resumed later by the
    // runtime (e.g. a sleep timer). Return values are not collected on the
    // async path (returns is ignored).
    bool call_service_method_coroutine(std::shared_ptr<LuaVM> vm,
                                       std::string_view method_name,
                                       const nlohmann::json& args,
                                       std::string* error = nullptr,
                                       uint64_t call_session = 0,
                                       LuaServiceManager* manager = nullptr,
                                       std::string_view service_id = "");

    // Invoke a named hook function on a service's module table.
    // The hook is called as hook(err, context_table) where context_table
    // has fields {type, method}. Returns true if the hook existed and was
    // called without error; false otherwise.
    bool invoke_hook(std::shared_ptr<LuaVM> vm,
                     const char* hook_name,
                     const std::string& err_or_reason,
                     const std::string& error_type,
                     const std::string& method_name);

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

    // Get service manager for this runtime
    LuaServiceManager* service_manager() const;

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

    // Clear script cache
    void clear_cache();

    // Get cache statistics
    size_t cache_size() const;
    LuaCacheConfig cache_config() const;

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
