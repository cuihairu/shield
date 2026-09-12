// [SHIELD_LUA] Lua service interface
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
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
class actor;
class actor_system;
}  // namespace caf

namespace shield::lua {

class LuaRuntime;
class LuaVM;
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
    // lifecycle method names like on_client_bound while preserving mailbox
    // dispatch.
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

    // Exit a service. `on_exit` runs on the service's actor thread via a
    // structured exit request; `deadline` (steady clock, from shutdown_all)
    // bounds how long the caller waits for it. When the deadline passes the
    // hung teardown takes over: the VM is never destroyed while its on_exit
    // may still be running, so the service is retired without touching any
    // of its sol state. nullopt means wait without a deadline.
    void exit(std::string_view service_id, std::string_view reason = "normal",
              std::optional<std::chrono::steady_clock::time_point> deadline =
                  std::nullopt);

    // Exit all services in reverse spawn order. `stop_budget_ms` bounds the
    // whole graceful phase: every service gets on_exit on its own actor
    // thread, but the total wait may not exceed the budget — once it is
    // exhausted, the remaining services take the force path (no on_exit;
    // registry removal + actor kill), and a service whose on_exit outlives
    // its share of the budget is retired by the hung teardown (no sol state
    // is destroyed underneath the stuck actor thread). <= 0 means unbounded.
    void shutdown_all(std::string_view reason = "stopping",
                      int64_t stop_budget_ms = 0);

    // Total forked tasks still pending across all services (drain signal).
    size_t pending_task_count_total() const;

    // The actor system every service actor lives on. Bootstrap uses it to
    // spawn one gateway actor per listener (see gateway_actor.hpp).
    caf::actor_system& actor_system() const;

    // Gateway actors by gateway name (the listener owner actor name). Client
    // contexts carry this name as gateway_address so egress/bind requests
    // find the gateway actor that owns the session.
    void register_gateway_actor(std::string gateway_name, caf::actor actor);
    caf::actor gateway_actor(std::string_view gateway_name) const;

    // A running service's actor by service name (or alias), nullptr when the
    // service does not exist. The gateway bridge uses it to deliver typed
    // ClientIngress / ClientControlMessage directly to the target's mailbox.
    caf::actor service_actor(std::string_view service_name) const;

    // Get current service ID
    std::string current_service_id() const;

    // Get the VM bound to a running service (nullptr if not found). The
    // returned pointer keeps the VM alive; touch it only from that service's
    // dispatch thread.
    std::shared_ptr<LuaVM> service_vm(std::string_view service_id) const;

    // Get current sender ID
    std::string current_sender_id() const;

    // Get current trace ID (empty if not in a message handler)
    std::string current_trace_id() const;

    // Get current call deadline in ms (0 if not set)
    int64_t current_deadline_ms() const;

    // Request that the currently running service exits after its handler
    // returns.
    void request_current_exit(std::string_view reason = "normal");

    // Consume a pending shield.exit request for a service (if any) and run
    // its exit path. Called at coroutine segment completion points — dispatch
    // completion, spawn init completion, and every resume of a suspended
    // handler — because the requesting dispatch frame may already be gone
    // (the tail of a yielded on_init runs on a caller-actor resume frame).
    // Returns true when an exit was driven.
    bool finish_pending_exit(const std::string& service_id);

    // Trigger the panic path for the currently running service: invoke its
    // on_panic(reason, {type="explicit"}) hook (best-effort) and request exit
    // with reason "panic". No-op outside a dispatch context.
    void panic_current(std::string_view reason);

    // Async spawn support. shield.spawn called inside a handler coroutine
    // suspends the caller (via suspend_for_call) and enqueues the blocking
    // part (VM creation + module load + on_init) onto a dedicated spawn
    // worker thread so the caller's service actor stays responsive.
    //
    // enqueue_async_spawn: queue spawn work correlated with `session`.
    // Returns false when the runtime is stopping (caller must fail the
    // session itself).
    bool enqueue_async_spawn(uint64_t session, std::string module,
                             std::string opts_json);

    // Query a published local service name.
    std::string query_service(std::string_view name) const;

    // Publish/unpublish additional local names owned by the current service.
    bool register_name(std::string_view name, std::string* error = nullptr);
    bool unregister_name(std::string_view name, std::string* error = nullptr);

    // Observe service-name publication changes (spawn publish, register/
    // unregister, service exit retraction). The notifier receives
    // (name, service_id); an empty service_id means the name was retracted.
    // Invoked outside the registry lock after each committed change. Set once
    // at bootstrap before any service spawns (e.g. cluster route
    // advertisement); unsettable notifier simply observes nothing.
    void set_name_change_notifier(
        std::function<void(const std::string&, const std::string&)> fn);

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

    // Yield handshake (called by every coroutine resume source once
    // lua_resume returned LUA_YIELD): publishes that the coroutine driving
    // `co` has observed the suspension, so a completion that already arrived
    // on another thread may safely lua_resume it. Until this fires,
    // resume_caller waits — resuming a still-running coroutine would fail and
    // lose the response with no recovery source.
    void mark_call_yielded(lua_State* co);

    // Record that the handler running on `co` is servicing a call request with
    // `session`, so its completion can be routed back to the caller.
    void set_handler_call_session(lua_State* co, uint64_t session);

    // Called when a handler coroutine finishes. If it was servicing a call
    // request, resume the suspended caller with the callee's return values.
    void on_handler_completed(lua_State* co,
                              const nlohmann::json& return_values);

    // Called when a handler coroutine fails while servicing a call request.
    void on_handler_failed(lua_State* co, const std::string& error_message);

    // Complete a call session from the callee side. Synchronous calls are
    // completed immediately; coroutine call responses are routed to the caller
    // service actor before resuming the caller coroutine.
    void complete_call(uint64_t session, bool ok, const nlohmann::json& values);

    // -- Proxied (remotely originated) call sessions (M4) --------------------
    // A cross-node call envelope that lands on this node is dispatched under
    // a locally-allocated "proxied" session occupying pending_calls with
    // caller_co == nullptr. The normal completion machinery routes its
    // outcome into the proxied hook (which replies over the transport)
    // instead of resuming a coroutine.

    // Allocate a proxied session for an incoming remote call. The deadline is
    // the caller's remaining timeout (floored) plus slack, so a vanished
    // remote caller cannot leak the entry forever. Thread-safe; never
    // returns 0.
    uint64_t begin_proxied_call(int32_t timeout_ms);

    // Drop a proxied session whose local dispatch failed before its handler
    // could run. Thread-safe; no-op for unknown or non-proxied sessions.
    void abandon_proxied_call(uint64_t session);

    // Completion hook for proxied sessions: invoked exactly once per session
    // with (session, ok, values) — from complete_call when the handler
    // finishes, or from check_call_timeouts on expiry. Thread-safe.
    void set_proxied_call_hook(
        std::function<void(uint64_t, bool, const nlohmann::json&)> hook);

    // Erase a proxied session and route its outcome through the hook.
    // Returns false when the session is gone or not proxied (the caller
    // falls back to the local coroutine-resume path).
    bool finish_proxied_call(uint64_t session, bool ok,
                             const nlohmann::json& values);

    // Run a call to completion on the calling thread: initiate(session)
    // starts the work (returning false with `error` set fails the call
    // immediately), then this blocks on the pending_sync_calls completion
    // path until the handler finishes or timeout_ms elapses. Used by the
    // remote sync-call path (main thread calling a service on another node).
    CallResult call_with_session(
        const std::function<bool(uint64_t, std::string&)>& initiate,
        int32_t timeout_ms);

    // Callee-side entry for an inbound remote call envelope (M4): allocates
    // a proxied session, dispatches the request to the local service, and
    // arms a self-contained expiry driver (caller timeout + slack). Returns
    // the proxied session, or 0 when dispatch failed immediately (error
    // set). Thread-safe.
    uint64_t dispatch_remote_call(std::string_view service_id,
                                  std::string_view method,
                                  const nlohmann::json& args,
                                  int32_t timeout_ms, std::string* error);

    // Phase 2 of the split callee-side entry: dispatch a session allocated
    // by begin_proxied_call() (the transport registers the envelope's
    // routing entry between the two phases so a fast callee completion can
    // never race it). Sends the call request and arms the expiry driver;
    // returns false (error set) when the target service is gone, in which
    // case the caller must abandon_proxied_call() the session. Thread-safe.
    bool dispatch_proxied_call(uint64_t session, std::string_view service_id,
                               std::string_view method,
                               const nlohmann::json& args, std::string* error);

    // Arm the expiry driver for a proxied session. Unlike the local-call
    // driver it does not target a caller actor (there is none): on fire it
    // fails the proxied session directly through the hook. Registered in the
    // same table as local drivers, so shutdown_all tears it down. Public for
    // tests that need a shorter fuse than the default slack.
    void schedule_proxied_call_timeout(uint64_t session, int32_t timeout_ms);

    // Resume a suspended caller (looked up by session) with the given result
    // values (or an error). Used by the caller actor after a response is routed
    // back to it, and by timeouts fired on the caller actor.
    void resume_caller(uint64_t session, bool ok, const nlohmann::json& values);

    // Check whether the current dispatch context is inside an on_exit handler.
    // Used by shield.call / shield.call_timeout to reject calls during exit.
    bool is_in_exit() const;

    // True on the thread running a spawn's on_init initial segment. Used by
    // shield.spawn to resolve synchronously inside on_init (the spawning
    // thread blocks, never a service actor). False on actor threads that
    // resume a yielded on_init, so a post-yield spawn stays async.
    static bool spawn_init_in_progress();

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
    /// MUST be called from the owning service actor's dispatch context (e.g.
    /// inside a fork task callback). Compiles the code with luaL_loadbuffer,
    /// executes it, and converts all return values to JSON.
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
    // External-waiter timeout driver: completes a manager.call() /
    // call_with_session() session with a timeout error via complete_call when
    // the delay elapses (the caller thread waits on the waiter's CV). Returns
    // false when the driver could not be armed (the caller then falls back to
    // a timed wait).
    bool schedule_external_call_timeout(int32_t timeout_ms, uint64_t session);
    bool cancel_actor_call_timeout(uint64_t session);

private:
    // Worker loop executing queued spawn jobs one at a time. Started in the
    // constructor, stopped and joined in the destructor before any actor/VM
    // teardown begins.
    void spawn_worker_loop();

    // Complete an async spawn session from the spawn worker thread. Routes
    // the result to the caller actor via CallResponseMessage (same channel
    // as coroutine call responses). If the caller already timed out, a
    // successfully spawned child is exited again ("timeout") to honor the
    // documented init-timeout contract.
    void finish_async_spawn(uint64_t session, const SpawnResult& result);

    // Force removal used by shutdown_all once the graceful budget is spent:
    // registry teardown + actor kill without invoking on_exit.
    void force_remove(const std::string& id, const std::string& reason);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::lua
