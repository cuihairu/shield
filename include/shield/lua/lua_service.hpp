// [SHIELD_LUA] Lua service interface
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <sol/forward.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shield/lua/clock.hpp"

// Lua C type forward declaration (declared in the global namespace; the
// runtime headers pull in the real definition, only the pointer is needed in
// this interface).
struct lua_State;

namespace caf {
class actor_system;
}  // namespace caf

namespace shield::lua {

class LuaRuntime;
class ServiceContext;

/// @brief Result of spawning a Lua service
struct SpawnResult {
    bool success;
    std::string service_id;
    std::string error_message;

    static SpawnResult ok(std::string id) { return {true, std::move(id), ""}; }

    static SpawnResult error(std::string msg) {
        return {false, "", std::move(msg)};
    }
};

/// @brief Result of dispatching a Lua service method.
struct CallResult {
    bool success;
    nlohmann::json values;
    std::string error_message;

    static CallResult ok(nlohmann::json values);
    static CallResult error(std::string msg);
};

/// @brief Lua service manager
/// Handles loading and spawning Lua services
class LuaServiceManager {
public:
    explicit LuaServiceManager(LuaRuntime& runtime, caf::actor_system& system);
    ~LuaServiceManager();

    // Spawn a service from a Lua module
    /// @param module Module name (e.g., "services.player")
    /// @param opts Optional spawn options (name, args, timeout)
    SpawnResult spawn(std::string_view module,
                      std::string_view opts_json = "{}");

    // Send a message to a service
    bool send(std::string_view target, std::string_view method,
              const nlohmann::json& args, std::string* error = nullptr);

    // Queue a runtime-owned event to a service. This is for internal bridges
    // such as shield_net -> Lua gateway callbacks, so it permits reserved
    // lifecycle method names like on_connect while preserving mailbox dispatch.
    bool send_system(std::string_view target, std::string_view method,
                     const nlohmann::json& args, std::string* error = nullptr);

    // Send a coroutine call-request message to a service. Like send() but tags
    // the message with a call session so the callee's dispatch can route the
    // response back to the suspended caller.
    bool send_call_request(std::string_view target, std::string_view method,
                           const nlohmann::json& args, uint64_t session,
                           std::string* error = nullptr);

    // Call a service and wait for response
    /// @return Response payload values or error
    CallResult call(std::string_view target, std::string_view method,
                    const nlohmann::json& args, int32_t timeout_ms = 5000);

    // Exit a service
    void exit(std::string_view service_id, std::string_view reason = "normal");

    // Exit all services in reverse spawn order.
    void shutdown_all(std::string_view reason = "stopping");

    // Get current service ID
    std::string current_service_id() const;

    // Get current sender ID
    std::string current_sender_id() const;

    // Get current trace ID (empty if not in a message handler)
    std::string current_trace_id() const;

    // Get current call deadline in ms (0 if not set)
    int64_t current_deadline_ms() const;

    // Request that the currently running service exits after its handler
    // returns.
    void request_current_exit(std::string_view reason = "normal");

    // Query a published local service name.
    std::string query_service(std::string_view name) const;

    // Publish/unpublish additional local names owned by the current service.
    bool register_name(std::string_view name, std::string* error = nullptr);
    bool unregister_name(std::string_view name, std::string* error = nullptr);

    // List registered services
    std::vector<std::string> list_services() const;

    // Enqueue a forked task to be executed by the owning service actor. The
    // task captures the owning service ID so it can be cancelled on service
    // exit.
    /// @param service_id Owner service ID
    /// @param task Function to execute on the service actor
    /// @return Task ID for cancellation/tracking
    uint64_t enqueue_forked_task(std::string service_id,
                                 std::function<void()> task);

    // Overload that also stores the raw Lua function for coroutine wrapping.
    uint64_t enqueue_forked_task(std::string service_id,
                                 std::function<void()> task,
                                 sol::function raw_fn);

    // Get the number of pending forked tasks for a service.
    size_t pending_task_count(const std::string& service_id) const;

    // Cancel all forked tasks owned by a service. Called during service exit.
    void cancel_forked_tasks_for_service(const std::string& service_id);

    // Coroutine-aware call support (GAP-010).
    //
    // suspend_for_call: anchor the current handler's coroutine and register a
    // pending call wait keyed by a fresh session id. The Lua caller then
    // coroutine.yield()s; the runtime resumes it via resume_caller once the
    // callee completes (or on timeout).
    uint64_t suspend_for_call(lua_State* caller_co, int32_t timeout_ms);

    // Record that the handler running on `co` is servicing a call request with
    // `session`, so its completion can be routed back to the caller.
    void set_handler_call_session(lua_State* co, uint64_t session);

    // Called when a handler coroutine finishes. If it was servicing a call
    // request, resume the suspended caller with the callee's return values.
    void on_handler_completed(lua_State* co,
                              const nlohmann::json& return_values);

    // Resume a suspended caller (looked up by session) with the given result
    // values (or an error). Used by the response path and timeouts.
    void resume_caller(uint64_t session, bool ok, const nlohmann::json& values);

    // Check whether the current dispatch context is inside an on_exit handler.
    // Used by shield.call / shield.call_timeout to reject calls during exit.
    bool is_in_exit() const;

    // Scan pending_calls for expired deadlines and resume each timed-out
    // caller with (false, {code="timeout", message="call timeout"}).
    /// @param now_ms Current monotonic time in milliseconds
    /// @return Number of calls timed out
    int check_call_timeouts(int64_t now_ms);

    // Invoke on_error(err, context) on the owning service's table (if the
    // function exists), increment the per-service error counter, and invoke
    // on_panic(reason, context) if the counter reaches the configured limit.
    /// @param service_id The service that encountered the error
    /// @param error_type Context type: "handler", "timer", or "fork"
    /// @param method_name Handler method name (empty for timer/fork)
    /// @param error_message The error message
    void invoke_error_hook(const std::string& service_id,
                           const std::string& error_type,
                           const std::string& method_name,
                           const std::string& error_message);

    // Reset the consecutive error counter for a service (called on success).
    void reset_error_count(const std::string& service_id);

    /// @brief Execute arbitrary Lua code on a service's VM.
    ///
    /// MUST be called from the worker thread (e.g. inside enqueue_forked_task).
    /// Compiles the code with luaL_loadbuffer, executes it, and converts
    /// all return values to JSON.
    ///
    /// @param service_id The service whose VM to execute in
    /// @param code Lua source code string
    /// @param result Output: JSON array of return values (empty if none)
    /// @param error Output: error message on failure
    /// @return true if compilation and execution succeeded
    bool exec_lua(const std::string& service_id, const std::string& code,
                  nlohmann::json* result = nullptr,
                  std::string* error = nullptr);

    // Attach a business-time clock so Lua code reads wall-clock UTC via
    // shield.now() / os.time() / os.date() (no-arg). When not attached the
    // default SystemClock is used. Tests inject MockClock to control time.
    void attach_clock(std::shared_ptr<Clock> clock);

    // Read the current business-time clock (wall-clock UTC).
    // Default SystemClock when no clock has been attached.
    int64_t clock_now_ms() const;
    int64_t clock_now_seconds() const;

    // CAF-backed timer helpers used by the Lua API when a service actor exists.
    uint64_t schedule_actor_timer_once(int64_t delay_ms, sol::function callback,
                                       const std::string& service_id);
    uint64_t schedule_actor_timer_once_fn(int64_t delay_ms,
                                          std::function<void()> callback,
                                          const std::string& service_id);
    uint64_t schedule_actor_timer_fixed_delay(int64_t interval_ms,
                                              sol::function callback,
                                              const std::string& service_id);
    bool cancel_actor_timer(uint64_t id);
    size_t active_actor_timer_count() const;

    // CAF-backed call-timeout bookkeeping. When a coroutine call is suspended
    // under an attached CAF actor system, a delayed event is scheduled to
    // resume the caller with a timeout error unless the call completes first.
    uint64_t schedule_actor_call_timeout(int32_t timeout_ms,
                                         const std::string& service_id,
                                         uint64_t session);
    bool cancel_actor_call_timeout(uint64_t session);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::lua
