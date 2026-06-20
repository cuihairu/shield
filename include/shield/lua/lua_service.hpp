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

// Lua C type forward declaration (declared in the global namespace; the
// runtime headers pull in the real definition, only the pointer is needed in
// this interface).
struct lua_State;

namespace shield::lua {

class LuaRuntime;
class ServiceContext;
class Mailbox;

/// @brief Result of spawning a Lua service
struct SpawnResult {
    bool success;
    std::string service_id;
    std::string error_message;

    static SpawnResult ok(std::string id) {
        return {true, std::move(id), ""};
    }

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
    explicit LuaServiceManager(LuaRuntime& runtime);
    ~LuaServiceManager();

    // Spawn a service from a Lua module
    /// @param module Module name (e.g., "services.player")
    /// @param opts Optional spawn options (name, args, timeout)
    SpawnResult spawn(std::string_view module,
                     std::string_view opts_json = "{}");

    // Send a message to a service
    bool send(std::string_view target, std::string_view method,
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

    // Request that the currently running service exits after its handler returns.
    void request_current_exit(std::string_view reason = "normal");

    // Query a published local service name.
    std::string query_service(std::string_view name) const;

    // Publish/unpublish additional local names owned by the current service.
    bool register_name(std::string_view name, std::string* error = nullptr);
    bool unregister_name(std::string_view name, std::string* error = nullptr);

    // List registered services
    std::vector<std::string> list_services() const;

    // Process next message from service's mailbox
    /// @param service_id Service to process
    /// @return true if a message was processed, false if mailbox was empty
    bool process_mailbox(std::string_view service_id);

    // Process one message from all services (round-robin)
    /// @return Number of messages processed
    int process_all_mailboxes();

    // Execute a single runtime pump: drain mailboxes, fire expired timers,
    // and timeout expired coroutines. Must be called from the worker thread
    // or from a test harness that owns the runtime.
    /// @return Number of events processed (messages + timers + timeouts)
    int pump_once();

    // Start the background worker thread that drives mailboxes, timers, and
    // coroutine timeouts. After this call, all Lua code (except shield.call
    // reentry from inside a handler on the worker thread) runs on the worker.
    void start_worker();

    // Stop the background worker thread. Joins the thread and clears pending
    // forked tasks. Safe to call multiple times; safe to call without start.
    void stop_worker();

    // Enqueue a forked task to be executed by the worker thread. The task
    // captures the owning service ID so it can be cancelled on service exit.
    /// @param service_id Owner service ID
    /// @param task Function to execute on the worker thread
    /// @return Task ID for cancellation/tracking
    uint64_t enqueue_forked_task(std::string service_id,
                                 std::function<void()> task);

    // Overload that also stores the raw Lua function for coroutine wrapping.
    uint64_t enqueue_forked_task(std::string service_id,
                                 std::function<void()> task,
                                 sol::function raw_fn);

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
    void on_handler_completed(lua_State* co, const nlohmann::json& return_values);

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::lua
