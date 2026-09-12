// [SHIELD_LUA] Lua service implementation
#include "shield/lua/lua_service.hpp"

#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/mail_cache.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shield/base/error.hpp"
#include "shield/base/result.hpp"
#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/plugin/plugin_host.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::lua {

namespace {
// Re-anchor a sol reference onto the main thread's lua_State. Handlers
// registered from inside a coroutine (on_init and every service handler run
// as one) carry that coroutine's lua_State, which the GC may collect once
// the coroutine finishes; a stored lua_State* would then dangle and any
// later use (luaL_unref from the cancel/erase paths) would crash. The
// registry is shared across all threads of one global_State, so re-refering
// the same registry entry from the main thread keeps the stored pointer
// alive without changing which value the reference names.
sol::function anchor_to_main_thread(sol::function fn) {
    lua_State* state = fn.lua_state();
    if (state == nullptr || !fn.valid()) {
        return fn;
    }
    lua_State* main_state = sol::main_thread(state);
    if (main_state == nullptr || main_state == state) {
        return fn;
    }
    return sol::function(main_state, sol::ref_index(fn.registry_index()));
}

struct DispatchFrame {
    std::string service_id;
    std::string sender_id;
    std::string trace_id;
    int64_t deadline_ms = 0;
    bool in_exit = false;
};

thread_local std::vector<DispatchFrame> tls_dispatch_stack;

}  // namespace

}  // namespace shield::lua

namespace shield::lua {

CallResult CallResult::ok(nlohmann::json values) {
    return {true, std::move(values), ""};
}

CallResult CallResult::error(std::string msg) {
    return {false, nlohmann::json::array(), std::move(msg)};
}

// True on the thread currently running a spawn's on_init (the spawning
// thread, or the spawn worker for async spawns). shield.spawn inside on_init
// takes the synchronous path: the child's init blocks the spawning thread —
// exactly the pre-coroutine behavior — while a spawn from a resumed init
// coroutine (actor thread) keeps the async path. Not set on the actor thread
// that resumes a yielded on_init, so a post-yield spawn never blocks an
// actor.
thread_local bool t_in_spawn_init = false;

struct LuaServiceManager::Impl {
    LuaRuntime& runtime;
    caf::actor_system& system;
    std::unordered_map<std::string, std::shared_ptr<LuaVM>> services;
    // VMs of services whose on_init is still running (registered by spawn
    // before the init coroutine starts, erased when the spawn finishes or
    // rolls back). Timer/fork dispatch during init resolves the VM here: the
    // suspended init coroutine must be resumable via shield.sleep /
    // shield.call even though the service is not published yet. Guarded by
    // registry_mutex like services.
    std::unordered_map<std::string, std::shared_ptr<LuaVM>> init_vms;
    std::unordered_map<std::string, std::string> published_names;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        owned_names;
    std::unordered_map<std::string, std::string> module_scripts;
    std::vector<std::string> service_order;

    // Per-service compiled client RPC state. Populated during spawn from
    // opts["rpc"]["routes"]: the descriptors owned by this service plus the
    // route_id -> Lua handler table for its inbound (c2s/bidi) bindings,
    // resolved once at startup (a missing handler fails the spawn, so
    // dispatch never resolves handlers dynamically). Guarded by
    // registry_mutex like the other per-service registry maps. Must be
    // erased BEFORE the owning VM in services (sol handles reference the
    // VM's lua_State).
    struct ServiceRpcState {
        shield::transport::RpcDescriptorTable descriptors;
        std::unordered_map<std::uint32_t, sol::function> handlers;
    };
    std::unordered_map<std::string, ServiceRpcState> service_rpc;

    // Pending shield.exit requests, keyed by service id. The requesting
    // dispatch frame may pop before the dispatcher looks for the request
    // (the tail of a yielded on_init / handler runs on a resume frame on
    // another actor thread), so a thread-local flag alone would lose it.
    // The dispatcher (dispatch_message / spawn / every coroutine resume
    // completion point) consumes the entry and drives exit() from there.
    std::mutex exit_request_mutex;
    std::unordered_map<std::string, std::string> service_exit_requests;

    // Records a pending exit request for a service (unless it is already
    // exiting). Called with the dispatch frame's service id.
    void request_exit_for(const std::string& service_id, std::string reason) {
        if (service_id.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(exit_request_mutex);
        service_exit_requests[service_id] = std::move(reason);
    }

    // Consumes a pending exit request for a service: returns true (exactly
    // once) with the requested reason, or false when none is pending.
    bool consume_exit_request(const std::string& service_id,
                              std::string* reason) {
        std::lock_guard<std::mutex> lock(exit_request_mutex);
        auto it = service_exit_requests.find(service_id);
        if (it == service_exit_requests.end()) {
            return false;
        }
        if (reason) {
            *reason = std::move(it->second);
        }
        service_exit_requests.erase(it);
        return true;
    }

    mutable std::shared_mutex registry_mutex;

    // VM lookup for timer/fork dispatch: a published service wins; while a
    // spawn's on_init is still running the VM is only in init_vms.
    std::shared_ptr<LuaVM> find_dispatch_vm(const std::string& id) {
        std::shared_lock lock(registry_mutex);
        if (auto it = services.find(id); it != services.end()) {
            return it->second;
        }
        if (auto it = init_vms.find(id); it != init_vms.end()) {
            return it->second;
        }
        return nullptr;
    }

    // RAII over the init_vms entry: spawn registers the VM before on_init
    // starts; the guard erases it on every exit path (success, rollback,
    // exception). It also drops a pending exit request that on_init recorded
    // but the spawn never consumed (init failed after the request): a
    // respawned incarnation must not inherit it.
    struct InitVmRegistration {
        Impl& impl;
        const std::string& name;
        ~InitVmRegistration() {
            std::string dropped;
            impl.consume_exit_request(name, &dropped);
            std::unique_lock lock(impl.registry_mutex);
            impl.init_vms.erase(name);
        }
    };

    // RAII over the spawn-init thread flag (t_in_spawn_init): shield.spawn
    // inside the initial on_init segment resolves synchronously. Saves and
    // restores so a nested spawn's on_init does not clear the outer flag.
    struct SpawnInitFlag {
        bool prev = t_in_spawn_init;
        ~SpawnInitFlag() { t_in_spawn_init = prev; }
    };

    std::atomic<bool> stopping{
        false};  // set by shutdown_all, checked by send/call/spawn

    // Observer for name publication changes (set once at bootstrap, before
    // any spawn; read without the registry lock, invoked outside of it).
    std::function<void(const std::string&, const std::string&)>
        name_change_notifier;

    // Forward one committed name change to the notifier (never called while
    // holding registry_mutex).
    void notify_name_change(const std::string& name,
                            const std::string& service_id) {
        if (name_change_notifier) name_change_notifier(name, service_id);
    }

    // Internal message representation used between the CAF actor behavior and
    // the Lua dispatch path. Replaces the legacy Mailbox::Message.
    struct DispatchMessage {
        std::string sender;
        std::string method;
        nlohmann::json args;
        std::string trace_id;
        int64_t deadline_ms = 0;
        bool high_priority = false;
        int64_t timestamp_ms = 0;
        // Coroutine call correlation. Defaults describe a plain send.
        // Call-request: call_session != 0.
        uint64_t call_session = 0;
    };

    // Forked task queue. Tasks are enqueued by shield.fork and consumed when
    // the owning service actor receives a fork_task_atom message.
    struct ForkedTask {
        uint64_t id;
        std::string service_id;
        std::function<void()> fn;
        sol::function raw_fn;  // original Lua function, for coroutine wrapping
    };
    std::atomic<uint64_t> next_task_id{1};
    std::vector<ForkedTask> pending_tasks;
    std::unordered_map<std::string, std::unordered_set<uint64_t>>
        tasks_by_service;
    std::mutex task_mutex;

    // Coroutine call correlation. A call from a handler yields the caller's
    // coroutine; the callee runs and, on completion, resumes the caller with
    // the callee's return values.
    //
    // Yield handshake: suspend_for_call registers the caller coroutine before
    // the suspending wrapper reaches its coroutine.yield(). A completion that
    // arrives in that window (the sync _coro_call pre-dispatch completions,
    // or a callee that answers within a scheduling slice) must not resume a
    // coroutine that is still running on the registering thread — lua_resume
    // rejects that and the response would be lost with no recovery source
    // (the call's own timeout driver is cancelled by the completion). The
    // resume source therefore waits on this shared sync until the driving
    // thread has observed LUA_YIELD and marks it (mark_call_yielded).
    struct CallYieldSync {
        std::mutex mtx;
        std::condition_variable cv;
        bool yielded = false;
    };
    struct PendingCall {
        uint64_t session = 0;
        lua_State* caller_co = nullptr;
        int caller_anchor = LUA_NOREF;  // registry ref keeping caller_co alive
        int64_t deadline_ms = 0;
        std::string caller_service;
        // M4: remotely originated call. Completion routes through
        // proxied_call_hook (reply to the source node) instead of resuming a
        // coroutine; there is no local caller actor or timeout driver.
        bool proxied = false;
        // Shared with the resume source (copies of this entry keep the same
        // object alive). Null for proxied calls, which have no coroutine.
        std::shared_ptr<CallYieldSync> yield_sync;
    };
    std::atomic<uint64_t> next_call_session{1};
    std::unordered_map<uint64_t, PendingCall>
        pending_calls;  // session -> caller wait
    std::unordered_map<lua_State*, uint64_t>
        handler_call_session;  // callee co -> session

    // Per-service consecutive error counter for panic detection.
    // Reset on successful handler completion; incremented on uncaught error.
    // Guarded by error_mutex: error hooks run on different service actors.
    std::mutex error_mutex;
    std::unordered_map<std::string, int> error_counts;

    // Track recently exited services for service_dead error distinction.
    // Reads happen under registry_mutex (shared); writes under
    // registry_mutex (unique). Entries are removed when the same name is
    // respawned, and the set is capped to bound memory.
    static constexpr size_t kRecentlyExitedLimit = 4096;
    std::unordered_set<std::string> recently_exited;

    // Track last sender per service for context_expired detection.
    std::unordered_map<std::string, std::string> last_sender_per_service;

    // Permission check hook (Phase 2). When set, called before send/call.
    // Returns empty string if allowed, error code if denied.
    // GCOVR_EXCL_LINE markers below: no setter exists yet, so the hook is
    // never installed (unreachable scaffold until Phase 2 lands).
    std::function<std::string(const std::string& sender,
                              const std::string& target,
                              const std::string& method)>
        permission_check;

    // CAF actor system reference. LuaServiceManager now always requires a CAF
    // actor system; every spawned service owns a CAF actor handle.
    std::unordered_map<std::string, caf::actor> service_actors;

    // Gateway actors by gateway name (see gateway_actor.hpp). Populated by
    // bootstrap, one per listener.
    std::unordered_map<std::string, caf::actor> gateway_actors;

    struct ActorTimerState {
        uint64_t id = 0;
        int64_t interval_ms = 0;
        bool repeating = false;
        std::string service_id;
        sol::function raw_callback;
        std::function<void()> native_callback;
        bool has_native_callback = false;
        bool active = true;
        caf::actor driver;
    };
    std::unordered_map<uint64_t, ActorTimerState> actor_timers;
    std::unordered_map<std::string, std::unordered_set<uint64_t>>
        actor_timers_by_service;
    std::atomic<uint64_t> next_actor_timer_id{1};

    std::unordered_map<uint64_t, caf::actor> actor_call_timeouts;

    // Graveyard for retired CAF actor handles: one-shot timer drivers that
    // already fired, cancelled timer / call-timeout drivers, and service
    // actors erased from service_actors. CAF's monitor-down notification
    // that wait_for_actors joins on is raised while the actor's worker is
    // still unwinding its cleanup() (stream/attachable teardown runs after
    // the down message), so dropping the last reference on a service-actor,
    // caller, or foreign thread — even after wait_for_actors — can race that
    // unwind and destroy the actor storage underneath it (TSan-verified heap
    // corruption). Retired handles therefore live here; the Impl destructor
    // drains the graveyard with stop_and_wait_for_actors at a point where
    // nothing concurrent can touch the actors anymore.
    std::mutex retired_actors_mutex;
    std::vector<caf::actor> retired_actors;

    // Names reserved by in-flight spawn() calls (init not finished yet).
    // Reserved names are rejected by spawn pre-checks but are not visible to
    // query_service until published (docs: reserve -> publish state machine).
    std::unordered_set<std::string> reserved_names;

    // Dedicated spawn worker. shield.spawn invoked inside a handler coroutine
    // suspends the caller and queues the blocking part (VM creation + module
    // load + on_init) here so the caller's service actor stays responsive.
    struct SpawnJob {
        uint64_t session = 0;
        std::string module;
        std::string opts_json;
    };
    std::mutex spawn_mutex;
    std::condition_variable spawn_cv;
    std::deque<SpawnJob> spawn_queue;
    bool spawn_stop = false;
    std::thread spawn_thread;

    // Business-time clock (AD-07: layered time). Lua-facing shield.now(),
    // os.time(), os.date() (no-arg) all read this clock. Default SystemClock
    // (wall-clock UTC); tests inject MockClock via attach_clock().
    std::shared_ptr<Clock> clock_{std::make_shared<SystemClock>()};

    // Pending synchronous calls from outside the actor system (main thread).
    // manager->call() blocks on the CV until the actor dispatches the method
    // and signals completion. This serializes Lua execution through the actor
    // instead of bypassing it with synchronous reentry (AD-01 / Step 3).
    struct PendingSyncCall {
        uint64_t session = 0;
        std::mutex mtx;
        std::condition_variable cv;
        bool completed = false;
        bool ok = false;
        nlohmann::json values;
        std::string error;
    };
    std::unordered_map<uint64_t, std::shared_ptr<PendingSyncCall>>
        pending_sync_calls;

    // Completion hook for proxied (remotely originated) call sessions (M4).
    // Installed by the bootstrap glue; forwards to the transport so the
    // result reaches the node the envelope came from.
    std::function<void(uint64_t, bool, const nlohmann::json&)>
        proxied_call_hook;

    int64_t clock_now_ms() const {
        std::shared_lock lock(registry_mutex);
        return clock_->now_ms();
    }

    int64_t clock_now_seconds() const {
        std::shared_lock lock(registry_mutex);
        return clock_->now_seconds();
    }

    // Dispatch a CAF-native ServiceMessage. Converts the typed fields into an
    // internal DispatchMessage and routes to the existing dispatch_message
    // path.
    void dispatch_service_message(class LuaServiceManager* manager,
                                  const std::string& id,
                                  const ServiceMessage& msg) {
        DispatchMessage m;
        m.sender = msg.sender;
        m.method = msg.method;
        m.args = msg.args;
        m.trace_id = msg.trace_id;
        m.deadline_ms = msg.deadline_ms;
        m.high_priority = msg.priority == MessagePriority::High;
        m.timestamp_ms = msg.timestamp_ms;
        m.call_session = msg.call_session;
        (void)dispatch_message(manager, id, m);
    }

    void dispatch_call_response(class LuaServiceManager* manager,
                                const CallResponseMessage& msg) {
        manager->resume_caller(msg.session, msg.ok, msg.values);
    }

    // Route a validated client-to-server payload to the target VM's compiled
    // RPC handler. Unlike ServiceMessage dispatch there is no method-name
    // lookup: the spawn-time RPC table owns route_id -> handler, so an
    // unknown route here is a startup-contract violation and is dropped with
    // a warning (never queued or retried). The request value follows the
    // descriptor contract: a pipeline-decoded canonical message wins, else a
    // JSON codec decodes body_bytes, else the raw bytes travel as a string.
    void dispatch_client_ingress(class LuaServiceManager* manager,
                                 const std::string& id,
                                 const ClientIngress& msg) {
        std::shared_ptr<LuaVM> service;
        sol::function handler;
        ClientIngress normalized = msg;
        {
            std::shared_lock lock(registry_mutex);
            auto svc_it = services.find(id);
            if (svc_it == services.end()) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(log, "client rpc for unknown service " + id);
                return;
            }
            service = svc_it->second;
            auto rpc_it = service_rpc.find(id);
            if (rpc_it == service_rpc.end()) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(log, "client rpc table missing for " + id);
                return;
            }
            auto h_it = rpc_it->second.handlers.find(msg.route_id);
            if (h_it == rpc_it->second.handlers.end()) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(log, "client rpc route " +
                                            std::to_string(msg.route_id) +
                                            " not owned by " + id);
                return;
            }
            handler = h_it->second;
            if (!normalized.decoded_request) {
                const auto* descriptor =
                    rpc_it->second.descriptors.find(msg.route_id);
                const bool json_codec = descriptor == nullptr ||
                                        descriptor->request_codec.empty() ||
                                        descriptor->request_codec == "json";
                if (json_codec) {
                    auto parsed = nlohmann::json::parse(
                        normalized.body_bytes.begin(),
                        normalized.body_bytes.end(), nullptr, false);
                    normalized.decoded_request =
                        parsed.is_discarded()
                            ? nlohmann::json(
                                  std::string(normalized.body_bytes.begin(),
                                              normalized.body_bytes.end()))
                            : std::move(parsed);
                } else {
                    normalized.decoded_request = nlohmann::json(
                        std::string(normalized.body_bytes.begin(),
                                    normalized.body_bytes.end()));
                }
            }
        }
        std::string error;
        if (!runtime.invoke_client_rpc(service, handler, normalized, &error,
                                       manager, id)) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_WARNING(
                log, "client rpc dispatch failed on " + id + " route " +
                         std::to_string(msg.route_id) + ": " + error);
        }
    }

    // Session lifecycle notification from the gateway: map the control kind
    // to its Lua convention handler and reuse the ordinary dispatch path
    // (which materializes the client-context marker into the read-only
    // ClientContext userdata). Reserved kinds have no producer yet and are
    // dropped with a warning.
    void dispatch_client_control(class LuaServiceManager* manager,
                                 const std::string& id,
                                 const ClientControlMessage& msg) {
        std::string method;
        nlohmann::json args = nlohmann::json::array({msg.context.to_json()});
        switch (msg.kind) {
            case ClientControlMessage::Kind::Bound:
                method = "on_client_bound";
                break;
            case ClientControlMessage::Kind::Disconnected:
                method = "on_disconnect";
                args.push_back(msg.reason);
                break;
            case ClientControlMessage::Kind::Unbound:
                method = "on_client_unbound";
                args.push_back(msg.reason);
                break;
            case ClientControlMessage::Kind::Reconnected: {
                // Reserved: no producer in this milestone.
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_WARNING(log,
                                   "dropped Reconnected control for session " +
                                       std::to_string(msg.context.session_id));
                return;
            }
        }
        std::string error;
        if (!manager->send_system(id, method, args, &error)) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_WARNING(log, "Failed to queue " + method + ": " + error);
        }
    }

    bool dispatch_message(class LuaServiceManager* manager,
                          const std::string& id, const DispatchMessage& msg) {
        std::shared_ptr<LuaVM> service;
        {
            std::shared_lock lock(registry_mutex);
            auto service_it = services.find(id);
            if (service_it != services.end()) {
                service = service_it->second;
            }
        }
        if (!service) {
            return false;
        }

        DispatchScope scope(*this, id, msg.sender, false, msg.trace_id,
                            msg.deadline_ms);

        std::string error;
        if (!runtime.call_service_method_coroutine(
                service, msg.method, msg.args, &error, msg.call_session,
                manager, id)) {
            // Method failed - log error but continue processing other messages.
        }

        // Honor shield.exit requested by the handler (or by a resumed
        // continuation of it — see finish_pending_exit).
        manager->finish_pending_exit(id);

        return true;
    }

    void run_fork_task_now(class LuaServiceManager* manager,
                           const ForkedTask& task) {
        DispatchScope scope(*this, task.service_id, "", false);
        if (task.raw_fn.valid()) {
            // Coroutine-aware dispatch: the forked task may yield via
            // shield.sleep / shield.call; the actor thread returns to the
            // mailbox immediately and the suspended coroutine is resumed by
            // the runtime when its wait completes.
            std::string error;
            std::shared_ptr<LuaVM> service = find_dispatch_vm(task.service_id);
            if (service &&
                runtime.invoke_coroutine(service, task.raw_fn, {}, "fork", "",
                                         0, manager, task.service_id, &error)) {
                return;
            }
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_ERROR(log, "forked task " + std::to_string(task.id) +
                                      " error: " + error);
        } else {
            try {
                task.fn();
            } catch (const std::exception& e) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_ERROR(log, "forked task " + std::to_string(task.id) +
                                          " error: " + e.what());
                manager->invoke_error_hook(task.service_id, "fork", "",
                                           e.what());
            }
        }
    }

    void run_ready_fork_task(class LuaServiceManager* manager,
                             uint64_t task_id) {
        std::optional<ForkedTask> task;
        {
            std::lock_guard<std::mutex> lock(task_mutex);
            for (auto it = pending_tasks.begin(); it != pending_tasks.end();
                 ++it) {
                if (it->id == task_id) {
                    task = std::move(*it);
                    pending_tasks.erase(it);
                    break;
                }
            }
            if (task) {
                auto by_service_it = tasks_by_service.find(task->service_id);
                if (by_service_it != tasks_by_service.end()) {
                    by_service_it->second.erase(task->id);
                    if (by_service_it->second.empty()) {
                        tasks_by_service.erase(by_service_it);
                    }
                }
            }
        }
        if (task) {
            run_fork_task_now(manager, *task);
        }
    }

    void run_timer_callback_now(class LuaServiceManager* manager,
                                const std::string& service_id,
                                sol::function cb) {
        if (!cb.valid()) {
            return;
        }
        DispatchScope scope(*this, service_id, "", false);
        // Coroutine-aware dispatch: the callback may yield via shield.sleep /
        // shield.call; the actor thread returns to the mailbox immediately.
        std::string error;
        std::shared_ptr<LuaVM> service = find_dispatch_vm(service_id);
        if (!service) {
            return;
        }
        if (!runtime.invoke_coroutine(service, cb, {}, "timer", "", 0, manager,
                                      service_id, &error)) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_ERROR(log, "timer error: " + error);
        }
    }

    void fire_actor_timer(class LuaServiceManager* manager,
                          const std::string& service_id, uint64_t timer_id) {
        ActorTimerState timer;
        caf::actor retired_driver;
        bool found = false;
        {
            std::unique_lock lock(registry_mutex);
            auto it = actor_timers.find(timer_id);
            if (it != actor_timers.end() && it->second.active) {
                timer = it->second;
                // The fire path never carries the driver in the copied state:
                // its last external reference may only be released where the
                // driver is provably not running (after wait_for_actors on
                // retired_driver below) or by the stop_and_wait paths that
                // cancel it. Erasing a non-repeating entry drops one
                // reference — often the last one, and CAF then runs its
                // on_unreachable teardown right here while the driver's
                // worker thread is still inside the tick handler that sent
                // this very fire message (TSan-verified data race).
                timer.driver = caf::actor{};
                found = true;
                if (!it->second.repeating) {
                    retired_driver = std::move(it->second.driver);
                    actor_timers_by_service[it->second.service_id].erase(
                        timer_id);
                    actor_timers.erase(it);
                }
            }
        }
        if (!found) {
            return;
        }
        if (timer.has_native_callback) {
            // Native callbacks (the sleep resume) continue a suspended
            // coroutine on this actor thread. Re-establish the owning
            // service's dispatch context first: everything the continuation
            // does — another shield.call's caller_service stamp, shield.fork
            // / timer ownership, shield.self — resolves through the dispatch
            // stack, and an empty stack would mis-route them.
            DispatchScope scope(*this, service_id, "", false);
            timer.native_callback();
        } else {
            run_timer_callback_now(manager, service_id, timer.raw_callback);
        }
        // The one-shot driver quits itself after firing; wait for it to
        // terminate here so the graveyard below never parks a running actor.
        // Waiting cannot deadlock: the driver's tick handler only forwards
        // the fire message and quits — it never needs this service actor
        // again. Even so the last reference is NOT dropped on this thread:
        // wait_for_actors joins on CAF's monitor-down, which the driver's
        // worker raises before its message loop has fully unwound, so the
        // destructor here would still race that unwind. The handle goes to
        // the graveyard instead and dies with the Impl.
        if (retired_driver) {
            wait_for_actors({retired_driver});
            retire_actor(std::move(retired_driver));
        }
    }

    // Runs the service's on_exit handler on the actor thread (the
    // ServiceExitRequest handler). Mirrors the inline copy in exit(): errors
    // are swallowed, a nested shield.exit inside on_exit is ignored.
    void run_exit_handler(const std::string& service_id,
                          const std::string& reason) {
        std::shared_ptr<LuaVM> service = find_dispatch_vm(service_id);
        if (!service) {
            return;
        }
        std::string error;
        nlohmann::json args = nlohmann::json(reason);
        DispatchScope scope(*this, service_id, "", true);
        (void)runtime.call_service_function(service, "on_exit", args, &error);
    }

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    Impl(LuaRuntime& rt, caf::actor_system& sys) : runtime(rt), system(sys) {
        for (const auto& actor : shield::config::runtime_actors()) {
            module_scripts.emplace(actor.name, resolve_script_path(actor));
        }
    }

    void stop_and_wait_for_actors(const std::vector<caf::actor>& actors) {
        if (actors.empty()) {
            return;
        }
        for (const auto& actor : actors) {
            if (actor) {
                caf::anon_send_exit(actor, caf::exit_reason::user_shutdown);
            }
        }
        wait_for_actors(actors);
    }

    // Blocks until every listed actor has terminated. Unlike
    // stop_and_wait_for_actors this never sends an exit signal: used when an
    // actor terminates itself (the ServiceExitRequest handler quits after
    // running on_exit) and an exit signal racing that request would drop it.
    void wait_for_actors(const std::vector<caf::actor>& actors) {
        if (actors.empty()) {
            return;
        }
        caf::scoped_actor self{system};
        self->wait_for(actors);
    }

    // Parks a driver handle instead of destroying it on this thread. See the
    // graveyard comment on the member declaration: the release must not happen
    // while any CAF worker may still be inside the driver's message loop.
    void retire_actor(caf::actor&& handle) {
        if (!handle) {
            return;
        }
        std::lock_guard<std::mutex> lock(retired_actors_mutex);
        retired_actors.push_back(std::move(handle));
    }

    void retire_actors(std::vector<caf::actor>&& handles) {
        for (auto& handle : handles) {
            retire_actor(std::move(handle));
        }
    }

    bool collect_actor_timer_for_cancel(
        uint64_t id, std::vector<caf::actor>* actors_to_stop) {
        std::unique_lock lock(registry_mutex);
        auto it = actor_timers.find(id);
        if (it == actor_timers.end()) {
            return false;
        }
        if (it->second.driver) {
            // Move the driver out: the entry dies below while the driver
            // actor may still be running its tick handler. Releasing its last
            // external reference at that moment would trigger CAF's
            // on_unreachable teardown on this thread concurrently with that
            // handler; the reference must only be released after
            // stop_and_wait_for_actors joined the driver.
            actors_to_stop->push_back(std::move(it->second.driver));
        }
        auto by_service_it =
            actor_timers_by_service.find(it->second.service_id);
        if (by_service_it != actor_timers_by_service.end()) {
            by_service_it->second.erase(id);
            if (by_service_it->second.empty()) {
                actor_timers_by_service.erase(by_service_it);
            }
        }
        actor_timers.erase(it);
        return true;
    }

    static std::string resolve_script_path(
        const shield::config::RuntimeActorConfig& actor) {
        std::filesystem::path script(actor.script);
        if (script.is_absolute() || std::filesystem::exists(script)) {
            return script.string();
        }

        if (!actor.source_dir.empty()) {
            auto from_config = std::filesystem::path(actor.source_dir) / script;
            if (std::filesystem::exists(from_config)) {
                return from_config.string();
            }
        }

        // Try global lua.script_path from config
        std::string global_script_path =
            shield::config::get("lua.script_path", "scripts");
        auto from_global = std::filesystem::path(global_script_path) / script;
        if (std::filesystem::exists(from_global)) {
            return from_global.string();
        }

        return script.string();
    }

    std::string resolve_module(std::string_view module) const {
        auto it = module_scripts.find(std::string(module));
        if (it != module_scripts.end()) {
            return it->second;
        }
        return std::string(module);
    }

    std::string current_service_id() const {
        if (tls_dispatch_stack.empty()) {
            return "";
        }
        return tls_dispatch_stack.back().service_id;
    }

    std::string current_sender_id() const {
        if (tls_dispatch_stack.empty()) {
            return "";
        }
        return tls_dispatch_stack.back().sender_id;
    }

    std::string current_trace_id() const {
        if (tls_dispatch_stack.empty()) {
            return "";
        }
        return tls_dispatch_stack.back().trace_id;
    }

    int64_t current_deadline_ms() const {
        if (tls_dispatch_stack.empty()) {
            return 0;
        }
        return tls_dispatch_stack.back().deadline_ms;
    }

    static bool valid_name(std::string_view name) {
        if (name.empty() || name.size() > 64 || name.rfind("shield.", 0) == 0) {
            return false;
        }
        for (char ch : name) {
            const bool ok =
                (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    class DispatchScope {
    public:
        DispatchScope(Impl& impl, std::string service_id, std::string sender_id,
                      bool in_exit, std::string trace_id = "",
                      int64_t deadline_ms = 0)
            : impl_(impl), service_id_(service_id) {
            tls_dispatch_stack.push_back({
                std::move(service_id),
                std::move(sender_id),
                std::move(trace_id),
                deadline_ms,
                in_exit,
            });
        }

        ~DispatchScope() {
            // Save last sender for context_expired detection.
            if (!tls_dispatch_stack.empty()) {
                const auto& frame = tls_dispatch_stack.back();
                if (!frame.sender_id.empty()) {
                    std::unique_lock lock(impl_.registry_mutex);
                    impl_.last_sender_per_service[frame.service_id] =
                        frame.sender_id;
                }
            }
            tls_dispatch_stack.pop_back();
        }

        DispatchScope(const DispatchScope&) = delete;
        DispatchScope& operator=(const DispatchScope&) = delete;

    private:
        Impl& impl_;
        std::string service_id_;
    };
};

LuaServiceManager::LuaServiceManager(LuaRuntime& runtime,
                                     caf::actor_system& system)
    : impl_(std::make_unique<Impl>(runtime, system)) {
    runtime.set_service_manager(this);

    // Back the host_api lua_current_service_id / lua_post_to_service slots
    // so plugins re-enter Lua on the owning service actor instead of
    // touching a lua_State from plugin threads.
    shield::plugin::LuaServiceHooks hooks;
    hooks.current_service_id = [this]() { return current_service_id(); };
    hooks.post_to_service = [this](const std::string& service_id,
                                   std::function<void()> fn) {
        // Hook body: fires only when a plugin calls lua_post_to_service;
        // integration context.
        // GCOVR_EXCL_START
        return enqueue_forked_task(service_id, std::move(fn));
        // GCOVR_EXCL_STOP
    };
    shield::plugin::global_host().set_lua_service_hooks(std::move(hooks));

    impl_->spawn_thread =
        std::thread(&LuaServiceManager::spawn_worker_loop, this);
}

LuaServiceManager::~LuaServiceManager() {
    // Stop the spawn worker before any actor/VM teardown: a job running on
    // that thread touches the registry and may finish by routing a response
    // to a caller actor.
    std::deque<Impl::SpawnJob> dropped_jobs;
    {
        std::lock_guard lock(impl_->spawn_mutex);
        impl_->spawn_stop = true;
        dropped_jobs = std::move(impl_->spawn_queue);
        impl_->spawn_queue.clear();
    }
    impl_->spawn_cv.notify_all();
    if (impl_->spawn_thread.joinable()) {
        impl_->spawn_thread.join();
    }
    // Fail queued-but-never-started spawns. The caller actor may already be
    // gone, so drop the pending wait without touching the caller's lua_State
    // (its VM may be destroyed); a leaked registry ref in a dying VM is
    // harmless.
    for (const auto& job : dropped_jobs) {
        cancel_actor_call_timeout(job.session);
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.erase(job.session);
    }

    // Detach the plugin hooks first so plugin threads can no longer post new
    // work into this (dying) manager. A hook copied just before the clear
    // may still fire during teardown — that window exists only during
    // process shutdown.
    shield::plugin::global_host().set_lua_service_hooks({});

    // Cancel pending timer/fork callbacks for every owned service
    // before this manager's state (and the service VMs it owns) is destroyed.
    // Without this cleanup the actor_timers' sol::function/std::function
    // callbacks would be released after the owning lua_State is already
    // closed.
    std::vector<std::string> service_ids;
    std::vector<caf::actor> actors_to_stop;
    {
        std::shared_lock lock(impl_->registry_mutex);
        service_ids = impl_->service_order;
    }
    for (const auto& service_id : service_ids) {
        cancel_forked_tasks_for_service(service_id);
        std::vector<uint64_t> actor_timer_ids;
        {
            std::shared_lock lock(impl_->registry_mutex);
            auto it = impl_->actor_timers_by_service.find(service_id);
            if (it != impl_->actor_timers_by_service.end()) {
                actor_timer_ids.assign(it->second.begin(), it->second.end());
            }
        }
        for (auto timer_id : actor_timer_ids) {
            impl_->collect_actor_timer_for_cancel(timer_id, &actors_to_stop);
        }
    }

    // Tear down CAF actors before releasing Lua VMs. Their handlers capture
    // this manager and may still have queued timer/call messages.
    {
        std::unique_lock lock(impl_->registry_mutex);
        actors_to_stop.reserve(
            actors_to_stop.size() + impl_->service_actors.size() +
            impl_->actor_timers.size() + impl_->actor_call_timeouts.size());
        for (auto& [id, actor] : impl_->service_actors) {
            if (actor) {
                // Move: the clear() below would otherwise release the map
                // entry's reference while the actor may still be running.
                actors_to_stop.push_back(std::move(actor));
            }
        }
        impl_->service_actors.clear();
        for (auto& [id, timer] : impl_->actor_timers) {
            if (timer.driver) {
                // Move: the clear() below would otherwise release the map
                // entry's reference while the driver may still be running;
                // releasing ours after stop_and_wait_for_actors is safe.
                actors_to_stop.push_back(std::move(timer.driver));
            }
        }
        impl_->actor_timers.clear();
        impl_->actor_timers_by_service.clear();
        for (auto& [session, driver] : impl_->actor_call_timeouts) {
            if (driver) {
                // Same move-before-clear reasoning as the timer drivers.
                actors_to_stop.push_back(std::move(driver));
            }
        }
        impl_->actor_call_timeouts.clear();
    }
    impl_->stop_and_wait_for_actors(actors_to_stop);
    // Drop the joined drivers into the graveyard rather than destructing the
    // vector here: an entry whose driver already terminated is harmless, but
    // routing every release through the graveyard keeps one single policy.
    impl_->retire_actors(std::move(actors_to_stop));

    // Wake up any pending sync calls (manager->call() / call_with_session
    // blocked on a CV) so they don't hang forever during shutdown, then wait
    // until every waiter has left: unlike the shutdown_all copy this runs
    // right before impl_ dies, so a waiter still touching pending_sync_calls
    // after the notify would be a use-after-free.
    {
        for (;;) {
            {
                std::unique_lock lock(impl_->registry_mutex);
                if (impl_->pending_sync_calls.empty()) {
                    break;
                }
                for (auto& [session, pending] : impl_->pending_sync_calls) {
                    std::unique_lock lk(pending->mtx);
                    if (!pending->completed) {
                        pending->error = "runtime is stopping";
                        pending->ok = false;
                        pending->completed = true;
                        pending->cv.notify_one();
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Final graveyard drain: by now every live and retired driver has been
    // sent an exit signal and waited on (the retired_actors vector itself
    // may hold already-terminated one-shots plus drivers that were cancelled
    // while still armed). Nothing concurrent can touch them anymore — the
    // spawn worker is stopped and every service actor is gone — so the last
    // references may finally be released here.
    impl_->stop_and_wait_for_actors(impl_->retired_actors);
    impl_->retired_actors.clear();

    impl_->runtime.set_service_manager(nullptr);
}

SpawnResult LuaServiceManager::spawn(std::string_view module,
                                     std::string_view opts_json) {
    if (impl_->stopping.load()) {
        return SpawnResult::error("runtime is stopping");
    }
    try {
        // Parse options
        nlohmann::json opts = nlohmann::json::parse(opts_json);

        std::string service_name = opts.value("name", "");
        nlohmann::json args = opts.value("args", nlohmann::json::object());
        nlohmann::json config = opts.value("config", nlohmann::json::object());

        // Generate service ID if no name provided
        if (service_name.empty()) {
            service_name = module;
            service_name += ":";
            service_name +=
                std::to_string(std::hash<std::string_view>{}(module));
        }
        {
            std::unique_lock lock(impl_->registry_mutex);
            if (impl_->services.contains(service_name)) {
                return SpawnResult::error("service already exists: " +
                                          service_name);
            }
            if (impl_->published_names.contains(service_name)) {
                return SpawnResult::error("service name already exists: " +
                                          service_name);
            }
            if (impl_->reserved_names.contains(service_name)) {
                return SpawnResult::error("service name already reserved: " +
                                          service_name);
            }
        }
        if (!Impl::valid_name(service_name)) {
            return SpawnResult::error("invalid service name: " + service_name);
        }

        // Reserve the name for the whole init phase: concurrent spawns with
        // the same name fail fast, while query_service still cannot see the
        // name until it is published after a successful on_init. The guard
        // rolls the reservation back on every failure path; the success path
        // clears it under the publish lock below.
        struct NameReservation {
            Impl* impl;
            std::string name;
            bool published = false;
            ~NameReservation() {
                if (published) {
                    return;
                }
                std::unique_lock lock(impl->registry_mutex);
                impl->reserved_names.erase(name);
            }
        } reservation{impl_.get(), service_name};
        {
            std::unique_lock lock(impl_->registry_mutex);
            impl_->reserved_names.insert(service_name);
        }

        const std::string script_path = impl_->resolve_module(module);

        // Create VM and load module
        auto vm = impl_->runtime.create_vm();

        std::string error;
        // GCOVR_EXCL_START (register_api failure during spawn needs a
        // native plugin that fails registration; integration context)
        if (!impl_->runtime.register_api(vm, &error)) {
            return SpawnResult::error("Failed to register Lua API: " + error);
            // GCOVR_EXCL_STOP
        }
        if (!impl_->runtime.load_service_module(vm, script_path, &error)) {
            return SpawnResult::error("Failed to load module: " + script_path +
                                      ": " + error);
        }

        // Compile this actor's rpc.routes against the loaded module.
        // Startup is the single validation point: an inbound (c2s/bidi)
        // binding whose method is missing or not a function fails the spawn
        // here (handler_missing), so dispatch never resolves handlers
        // dynamically. s2c routes only need a non-empty binding (their
        // helpers are API-level). Entries owned by other services are not
        // compiled here; they exist in the gateway merge table only.
        Impl::ServiceRpcState rpc_state;
        {
            const nlohmann::json routes_json =
                opts.contains("rpc") && opts["rpc"].is_object() &&
                        opts["rpc"].contains("routes")
                    ? opts["rpc"]["routes"]
                    : nlohmann::json::array();
            shield::transport::RpcDescriptorTable descriptors;
            std::string rpc_error;
            if (!shield::transport::parse_rpc_routes_json(
                    routes_json.dump(), descriptors, &rpc_error)) {
                return SpawnResult::error("rpc.routes for " + service_name +
                                          ": " + rpc_error);
            }
            descriptors.for_each(
                [&](const shield::transport::RpcDescriptor& descriptor) {
                    if (!rpc_error.empty()) {
                        return;
                    }
                    if (!descriptor.owner_service.empty() &&
                        descriptor.owner_service != service_name) {
                        return;
                    }
                    if (descriptor.direction ==
                        shield::transport::RouteDirection::ServerToClient) {
                        // s2c helpers: publish shield.client_rpc.<binding> so
                        // the handler can push server-to-client payloads
                        // through the owning gateway actor (M2). Binding
                        // presence was already enforced by the descriptor
                        // parser.
                        (void)rpc_state.descriptors.add(descriptor);
                        register_client_rpc_helper(impl_->runtime.vm_state(vm),
                                                   this, descriptor.binding,
                                                   descriptor.route_id);
                        return;
                    }
                    sol::function handler;
                    if (!impl_->runtime.resolve_service_method(
                            vm, descriptor.binding, &handler, &rpc_error)) {
                        rpc_error = "route " +
                                    std::to_string(descriptor.route_id) +
                                    " binding '" + descriptor.binding +
                                    "': handler_missing (" + rpc_error + ")";
                        return;
                    }
                    (void)rpc_state.descriptors.add(descriptor);
                    rpc_state.handlers.emplace(descriptor.route_id,
                                               std::move(handler));
                });
            if (!rpc_error.empty()) {
                return SpawnResult::error("rpc binding compile failed for " +
                                          service_name + ": " + rpc_error);
            }
        }

        // Parse spawn timeout (default 10s).
        const int64_t spawn_timeout_ms = opts.value("timeout", 10000);

        // Create a CAF actor for this service before on_init so that
        // on_init-time fork/timer registration can immediately route through
        // the actor path. The service VM is published only after on_init
        // succeeds; until then the actor exists purely as an internal handle.
        //
        // The behavior pattern-matches the native typed messages
        // (ServiceMessage, CallResponseMessage, timer_fire_atom + uint64_t,
        // call_timeout_atom + uint64_t). No string/JSON dispatch remains.
        auto actor = impl_->system.spawn([impl_ptr = impl_.get(),
                                          manager = this, svc = service_name](
                                             caf::event_based_actor* self)
                                             -> caf::behavior {
            // Message stashing: until on_init completes on the spawning
            // thread, business messages are stashed so fork/timer/call
            // cannot touch this Lua VM concurrently with on_init. Runtime
            // driven resume messages pass through: sleep timers, call
            // responses and call timeouts can only arrive once on_init has
            // yielded, i.e. while the spawning thread is parked on the init
            // waiter and the VM is idle. fork_task_atom is deliberately NOT
            // passed through: a fork enqueued by a still synchronous on_init
            // would otherwise execute here while the spawning thread is still
            // inside the VM — two OS threads in one lua_State. Stashing it
            // defers execution to the unstash below, so a fork scheduled
            // during on_init runs serially after init (the documented fork
            // contract). The spawner sends init_ready_atom once on_init
            // returns; we then install the real behavior and release the
            // stash.
            auto cache = std::make_shared<caf::mail_cache>(self, 4096);
            self->set_default_handler(
                [cache, impl_ptr, manager,
                 svc](caf::message& msg) -> caf::skippable_result {
                    if (msg.size() == 2 &&
                        (msg.match_element<timer_fire_atom>(0) ||
                         msg.match_element<call_timeout_atom>(0))) {
                        const uint64_t payload = msg.get_as<uint64_t>(1);
                        if (msg.match_element<timer_fire_atom>(0)) {
                            impl_ptr->fire_actor_timer(manager, svc, payload);
                        } else {
                            manager->cancel_actor_call_timeout(payload);
                            nlohmann::json timeout_err =
                                nlohmann::json::array({nlohmann::json::object(
                                    {{"code", "timeout"},
                                     {"message", "call timeout"},
                                     {"retryable", true}})});
                            manager->resume_caller(payload, false, timeout_err);
                        }
                        return {};
                    }
                    if (msg.match_element<CallResponseMessage>(0)) {
                        impl_ptr->dispatch_call_response(
                            manager, msg.get_as<CallResponseMessage>(0));
                        return {};
                    }
                    cache->stash(msg);
                    return {};
                });
            return caf::behavior{
                [self, cache, impl_ptr, manager, svc](init_ready_atom) {
                    self->set_default_handler(caf::print_and_drop);
                    self->become(caf::behavior{
                        [impl_ptr, manager, svc](const ServiceMessage& msg) {
                            impl_ptr->dispatch_service_message(manager, svc,
                                                               msg);
                        },
                        [impl_ptr, manager, svc](const ClientIngress& msg) {
                            impl_ptr->dispatch_client_ingress(manager, svc,
                                                              msg);
                        },
                        [impl_ptr, manager,
                         svc](const ClientControlMessage& msg) {
                            impl_ptr->dispatch_client_control(manager, svc,
                                                              msg);
                        },
                        [impl_ptr, manager](const CallResponseMessage& msg) {
                            impl_ptr->dispatch_call_response(manager, msg);
                        },
                        [impl_ptr, manager, svc](timer_fire_atom,
                                                 uint64_t timer_id) {
                            impl_ptr->fire_actor_timer(manager, svc, timer_id);
                        },
                        [impl_ptr, manager, svc](call_timeout_atom,
                                                 uint64_t session) {
                            manager->cancel_actor_call_timeout(session);
                            nlohmann::json timeout_err =
                                nlohmann::json::array({nlohmann::json::object(
                                    {{"code", "timeout"},
                                     {"message", "call timeout"},
                                     {"retryable", true}})});
                            manager->resume_caller(session, false, timeout_err);
                        },
                        [impl_ptr, manager](fork_task_atom, uint64_t task_id) {
                            impl_ptr->run_ready_fork_task(manager, task_id);
                        },
                        [self, impl_ptr, svc](const ServiceExitRequest& req) {
                            // Structured exit from a foreign thread
                            // (manager.exit / shutdown_all): on_exit runs
                            // here on the actor thread — the only thread
                            // allowed to touch this VM — and the actor quits
                            // itself so the exiting thread can observe
                            // completion via wait_for instead of racing this
                            // thread with a direct VM call.
                            impl_ptr->run_exit_handler(svc, req.reason);
                            self->quit(caf::exit_reason::user_shutdown);
                        },
                    });
                    cache->unstash();
                },
            };
        });
        {
            std::unique_lock lock(impl_->registry_mutex);
            impl_->service_actors.emplace(service_name, actor);
            // Init-phase VM registration: timer/fork dispatch during on_init
            // resolves the VM here (find_dispatch_vm) so a shield.sleep or
            // fork inside on_init is resumable even though the service is
            // not published yet. Erased on every exit path by the guard.
            impl_->init_vms[service_name] = vm;
        }
        Impl::InitVmRegistration init_vm_guard{*impl_, service_name};

        nlohmann::json init_args = {
            {"name", service_name},
            {"id", service_name},
            {"args", args},
            {"config", config},
        };
        bool exit_after_init = false;
        std::string exit_reason;
        {
            Impl::DispatchScope scope(*impl_, service_name, "", false);
            // on_init runs as a coroutine so it may yield inside
            // shield.sleep / shield.call: the spawn waits on the external
            // waiter while the runtime resumes the coroutine. The spawn
            // timeout budget bounds the whole wait.
            Impl::SpawnInitFlag spawn_init_flag;
            t_in_spawn_init = true;
            sol::function on_init_fn;
            if (impl_->runtime.resolve_service_method(vm, "on_init",
                                                      &on_init_fn, &error)) {
                // An on_init that never yields (busy loop / pure sync code)
                // runs to completion inside the starter lambda below: the
                // only timeout signal is the elapsed measurement, exactly as
                // the synchronous dispatch did.
                const int64_t init_start = Impl::now_ms();
                auto init_result = call_with_session(
                    [&](uint64_t session, std::string& err) -> bool {
                        // on_init keeps its historical no-ctx signature:
                        // on_init(args) — the init arguments arrive directly.
                        return impl_->runtime.invoke_coroutine(
                            vm, on_init_fn, {init_args}, "handler", "on_init",
                            session, this, service_name, &err,
                            /*prepend_ctx=*/false);
                    },
                    static_cast<int32_t>(spawn_timeout_ms));
                const int64_t init_elapsed_ms = Impl::now_ms() - init_start;
                // A non-yielding on_init that overruns the budget cannot be
                // preempted: the elapsed measurement (not the CAF driver)
                // must win over a business false/nil return, matching the
                // synchronous dispatch semantics.
                const bool init_overran =
                    init_elapsed_ms >= static_cast<int64_t>(spawn_timeout_ms);
                if (!init_result.success) {
                    error = init_result.error_message;
                    if (error.find("call timeout") != std::string::npos ||
                        init_overran) {
                        return SpawnResult::error(
                            "spawn timeout: on_init exceeded " +
                            std::to_string(spawn_timeout_ms) + "ms limit");
                    }
                } else if (!init_result.values.empty()) {
                    // call_service_function contract: a hook returning
                    // (false, msg) or (nil, msg) reports a failure.
                    const nlohmann::json& first = init_result.values[0];
                    const bool false_first =
                        first.is_boolean() && !first.get<bool>();
                    const bool nil_first =
                        first.is_null() && init_result.values.size() > 1;
                    if (false_first || nil_first) {
                        if (init_overran) {
                            return SpawnResult::error(
                                "spawn timeout: on_init exceeded " +
                                std::to_string(spawn_timeout_ms) + "ms limit");
                        }
                        error = init_result.values.size() > 1 &&
                                        init_result.values[1].is_string()
                                    ? init_result.values[1].get<std::string>()
                                : false_first ? "on_init returned false"
                                              : "on_init returned nil";
                    }
                }
            } else {
                // A module without on_init is fine (call_service_function
                // treated a missing hook as a silent no-op); anything else is
                // a load failure surfaced below.
                if (error.find("is missing or not a function") ==
                    std::string::npos) {
                    return SpawnResult::error("on_init failed for " +
                                              service_name + ": " + error);
                }
                error.clear();
            }
            if (!error.empty()) {
                std::vector<std::string> retracted;
                {
                    std::unique_lock lock(impl_->registry_mutex);
                    if (auto actor_it =
                            impl_->service_actors.find(service_name);
                        actor_it != impl_->service_actors.end()) {
                        // The actor may still be inside the failed on_init on
                        // its worker thread; the graveyard keeps the handle
                        // alive until the Impl destructor joins it.
                        if (actor_it->second) {
                            caf::anon_send_exit(
                                actor_it->second,
                                caf::exit_reason::user_shutdown);
                        }
                        impl_->retire_actor(std::move(actor_it->second));
                        impl_->service_actors.erase(actor_it);
                    }
                    if (auto names_it = impl_->owned_names.find(service_name);
                        names_it != impl_->owned_names.end()) {
                        for (const auto& name : names_it->second) {
                            impl_->published_names.erase(name);
                            retracted.push_back(name);
                        }
                        impl_->owned_names.erase(names_it);
                    }
                }
                for (const auto& name : retracted) {
                    impl_->notify_name_change(name, "");
                }
                return SpawnResult::error("on_init failed for " + service_name +
                                          ": " + error);
            }
            // The tail of a yielded on_init runs on a caller-actor resume
            // frame (a foreign thread from this spawn), so a shield.exit
            // issued there is only visible through the shared pending-exit
            // store — consume it here, exactly once, after init succeeded.
            exit_after_init =
                impl_->consume_exit_request(service_name, &exit_reason);
        }

        {
            std::unique_lock lock(impl_->registry_mutex);
            if (impl_->services.contains(service_name) ||
                impl_->published_names.contains(service_name)) {
                if (auto actor_it = impl_->service_actors.find(service_name);
                    actor_it != impl_->service_actors.end()) {
                    // Graveyard policy: the actor may still be running its
                    // on_init; never release the last reference here.
                    if (actor_it->second) {
                        caf::anon_send_exit(actor_it->second,
                                            caf::exit_reason::user_shutdown);
                    }
                    impl_->retire_actor(std::move(actor_it->second));
                    impl_->service_actors.erase(actor_it);
                }
                return SpawnResult::error("service name already exists: " +
                                          service_name);
            }
            impl_->services[service_name] = std::move(vm);
            impl_->service_rpc[service_name] = std::move(rpc_state);
            impl_->published_names[service_name] = service_name;
            impl_->owned_names[service_name].insert(service_name);
            impl_->service_order.push_back(service_name);
            impl_->reserved_names.erase(service_name);
            // A respawned name is alive again: clear its exit tombstone so
            // senders get service_not_found-vs-dead distinction right.
            impl_->recently_exited.erase(service_name);
            reservation.published = true;
        }
        impl_->notify_name_change(service_name, service_name);

        // on_init succeeded: tell the actor to install its real behavior and
        // release any messages stashed during init (see spawn lambda above).
        caf::anon_send(actor, init_ready_atom_v);

        if (exit_after_init) {
            exit(service_name, exit_reason);
        }
        return SpawnResult::ok(service_name);

    } catch (const std::exception& e) {
        return SpawnResult::error(std::string("Spawn failed: ") + e.what());
    }
}

namespace {

bool validate_message_method(std::string_view method, bool allow_reserved,
                             std::string* error) {
    if (method.empty() || method.size() > 128) {
        if (error) *error = "invalid method name: length must be 1-128";
        return false;
    }
    if (!allow_reserved && method.rfind("on_", 0) == 0) {
        if (error) *error = "invalid method name: 'on_' prefix is reserved";
        return false;
    }
    return true;
}

bool validate_message_payload(const nlohmann::json& args, std::string* error) {
    const std::string serialized = args.dump();
    if (serialized.size() > kMaxMessageSize) {
        if (error) {
            *error = "message too large: " + std::to_string(serialized.size()) +
                     " bytes (max " + std::to_string(kMaxMessageSize) + ")";
        }
        return false;
    }
    // Check for unsupported JSON values, e.g. from lua_to_json returning the
    // sentinel string for userdata/functions.
    if (serialized.find("\"<unsupported>\"") != std::string::npos) {
        if (error) *error = "message contains unsupported value type";
        return false;
    }
    return true;
}

}  // namespace

bool LuaServiceManager::send(std::string_view target, std::string_view method,
                             const nlohmann::json& args, std::string* error) {
    if (impl_->stopping.load()) {
        if (error) *error = "runtime is stopping";
        return false;
    }

    if (!validate_message_method(method, false, error)) {
        return false;
    }

    // Permission check.
    // GCOVR_EXCL_START (Phase-2 scaffold: no setter exists yet)
    if (impl_->permission_check) {
        const std::string sender = current_service_id();
        const std::string denial = impl_->permission_check(
            sender, std::string(target), std::string(method));
        if (!denial.empty()) {
            if (error) *error = "permission denied: " + denial;
            return false;
        }
    }
    // GCOVR_EXCL_STOP

    if (!validate_message_payload(args, error)) {
        return false;
    }

    const std::string service_id = query_service(target);

    std::optional<caf::actor> actor_opt;
    bool target_recently_exited = false;
    {
        // Also read recently_exited under the same lock: exit() inserts
        // into it under the unique lock on another thread.
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it != impl_->service_actors.end()) {
            actor_opt = it->second;
        }
        target_recently_exited =
            impl_->recently_exited.count(service_id) > 0 ||
            impl_->recently_exited.count(std::string(target)) > 0;
    }
    if (!actor_opt) {
        if (error) {
            if (target_recently_exited) {
                *error = "service dead: " + std::string(target);
            } else {
                *error = "service not found: " + std::string(target);
            }
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    const std::string sender = current_service_id();
    ServiceMessage msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.priority = MessagePriority::Normal;
    msg.timestamp_ms = now_ms;
    caf::anon_send(*actor_opt, std::move(msg));
    return true;
}

bool LuaServiceManager::send_system(std::string_view target,
                                    std::string_view method,
                                    const nlohmann::json& args,
                                    std::string* error) {
    if (impl_->stopping.load()) {
        if (error) *error = "runtime is stopping";
        return false;
    }
    if (!validate_message_method(method, true, error) ||
        !validate_message_payload(args, error)) {
        return false;
    }

    const std::string service_id = query_service(target);

    std::optional<caf::actor> actor_opt;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it != impl_->service_actors.end()) {
            actor_opt = it->second;
        }
    }
    if (!actor_opt) {
        if (error) *error = "service not found: " + std::string(target);
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    ServiceMessage msg;
    msg.sender = "";
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = "";
    msg.deadline_ms = 0;
    msg.priority = MessagePriority::High;
    msg.timestamp_ms = now_ms;
    caf::anon_send(*actor_opt, std::move(msg));
    return true;
}

caf::actor LuaServiceManager::service_actor(
    std::string_view service_name) const {
    const std::string service_id = query_service(service_name);
    std::shared_lock lock(impl_->registry_mutex);
    auto it = impl_->service_actors.find(service_id);
    return it == impl_->service_actors.end() ? nullptr : it->second;
}

bool LuaServiceManager::send_call_request(std::string_view target,
                                          std::string_view method,
                                          const nlohmann::json& args,
                                          uint64_t session,
                                          std::string* error) {
    const std::string service_id = query_service(target);

    std::optional<caf::actor> actor_opt;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it != impl_->service_actors.end()) {
            actor_opt = it->second;
        }
    }
    if (!actor_opt) {
        if (error) {
            *error = "service not found: " + std::string(target);
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    const std::string sender = current_service_id();
    ServiceMessage msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.priority = MessagePriority::Normal;
    msg.timestamp_ms = now_ms;
    msg.call_session = session;
    caf::anon_send(*actor_opt, std::move(msg));
    return true;
}

CallResult LuaServiceManager::call(std::string_view target,
                                   std::string_view method,
                                   const nlohmann::json& args,
                                   int32_t timeout_ms) {
    if (impl_->stopping.load()) {
        return CallResult::error("runtime is stopping");
    }

    const std::string service_id = query_service(target);
    std::shared_ptr<LuaVM> service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto service_it = impl_->services.find(service_id);
        if (service_it != impl_->services.end()) {
            service = service_it->second;
        }
    }
    if (!service) {
        return CallResult::error("service not found: " + std::string(target));
    }

    const std::string sender = current_service_id();

    std::optional<caf::actor> actor_opt;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it != impl_->service_actors.end()) {
            actor_opt = it->second;
        }
    }
    if (!actor_opt) {
        return CallResult::error("service not found: " + std::string(target));
    }

    // Self-call detection: avoid deadlock (actor mailbox would queue but the
    // actor is currently executing this handler).
    if (sender == service_id) {
        return CallResult::error("self-call not supported");
    }

    // Create the pending external waiter, then send the call request through
    // the ordinary ServiceMessage path (call_session non-zero marks it as a
    // call; the callee's completion routes back via complete_call).
    const uint64_t session = impl_->next_call_session.fetch_add(1);
    auto pending = std::make_shared<Impl::PendingSyncCall>();
    pending->session = session;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls[session] = pending;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    ServiceMessage msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.timestamp_ms = now_ms;
    msg.call_session = session;

    // Send to target actor.
    caf::anon_send(*actor_opt, std::move(msg));

    // Block until the handler completes. The expiry is driven by the CAF
    // delayed driver (it completes the call with a timeout error); when the
    // driver could not be armed, fall back to a timed wait on this thread.
    const int32_t effective_timeout = timeout_ms > 0 ? timeout_ms : 5000;
    const bool driver_armed =
        schedule_external_call_timeout(effective_timeout, session);
    bool completed = false;
    {
        std::unique_lock lk(pending->mtx);
        if (driver_armed) {
            pending->cv.wait(lk, [&] { return pending->completed; });
        } else {
            completed = pending->cv.wait_for(
                lk, std::chrono::milliseconds(effective_timeout),
                [&] { return pending->completed; });
        }
        completed = completed || pending->completed;
    }

    // Cleanup.
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls.erase(session);
    }

    if (!completed) {
        return CallResult::error(
            "call timeout (actor dispatch exceeded limit)");
    }
    if (pending->ok) {
        return CallResult::ok(std::move(pending->values));
    }
    return CallResult::error(std::move(pending->error));
}

void LuaServiceManager::exit(std::string_view service_id,
                             std::string_view reason) {
    const std::string id(service_id);
    std::shared_ptr<LuaVM> service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->services.find(id);
        if (it != impl_->services.end()) {
            service = it->second;
        }
    }
    if (!service) {
        return;
    }

    const bool exiting_from_own_actor = current_service_id() == id;

    std::vector<caf::actor> actors_to_wait;
    if (exiting_from_own_actor) {
        // Own-actor exit: we are already on the service's actor thread
        // (inside a dispatch handler), so running on_exit inline is the
        // serialized continuation of the current message.
        std::string error;
        nlohmann::json args = std::string(reason);
        Impl::DispatchScope scope(*impl_, id, "", true);
        (void)impl_->runtime.call_service_function(service, "on_exit", args,
                                                   &error);
    } else {
        // Foreign-thread exit: never touch the VM here. Route on_exit
        // through the actor's mailbox (the ServiceExitRequest handler runs
        // it and quits the actor) and wait below — a direct call would race
        // the actor thread processing in-flight messages on the same
        // lua_State, which is heap corruption, not just a data race.
        std::optional<caf::actor> actor;
        {
            std::shared_lock lock(impl_->registry_mutex);
            auto it = impl_->service_actors.find(id);
            if (it != impl_->service_actors.end() && it->second) {
                actor = it->second;
            }
        }
        if (actor) {
            caf::anon_send(*actor, ServiceExitRequest{std::string(reason)});
            actors_to_wait.push_back(*actor);
        }
        // on_exit must complete before the teardown below: the handler
        // resolves its VM through the registry, and the VM's last owner
        // (the local `service` handle) dies with this frame.
        impl_->wait_for_actors(actors_to_wait);
        // wait_for_actors joins on CAF's monitor-down, which is raised while
        // the actor's worker is still unwinding cleanup() — releasing the
        // last reference on this thread would destroy the actor storage
        // underneath it (TSan-verified heap corruption). Park the handles in
        // the graveyard; the Impl destructor releases them safely.
        impl_->retire_actors(std::move(actors_to_wait));
    }

    // Cancel forked tasks / timers / coroutines BEFORE erasing the service
    // VM. These hold sol::function / std::function callbacks that reference
    // the service's lua_State; releasing them after the VM is destroyed
    // would luaL_unref on a closed state.
    cancel_forked_tasks_for_service(id);
    std::vector<caf::actor> actors_to_stop;
    std::vector<uint64_t> actor_timer_ids;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto timers_it = impl_->actor_timers_by_service.find(id);
        if (timers_it != impl_->actor_timers_by_service.end()) {
            actor_timer_ids.assign(timers_it->second.begin(),
                                   timers_it->second.end());
        }
    }
    for (auto timer_id : actor_timer_ids) {
        impl_->collect_actor_timer_for_cancel(timer_id, &actors_to_stop);
    }
    // Cancel any pending CAF call-timeout drivers for calls originated by this
    // service. The timeout path (call_timeout_atom handler) will
    // no-op if the session is already gone from pending_calls.
    {
        std::unique_lock lock(impl_->registry_mutex);
        for (auto it = impl_->actor_call_timeouts.begin();
             it != impl_->actor_call_timeouts.end();) {
            auto pc_it = impl_->pending_calls.find(it->first);
            if (pc_it != impl_->pending_calls.end() &&
                pc_it->second.caller_service == id) {
                if (it->second) {
                    // Move before erase: the entry's reference must not be
                    // released while the driver may still be running; ours
                    // drops after stop_and_wait_for_actors below.
                    actors_to_stop.push_back(std::move(it->second));
                }
                it = impl_->actor_call_timeouts.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<std::string> retracted;
    {
        std::unique_lock lock(impl_->registry_mutex);
        if (auto names_it = impl_->owned_names.find(id);
            names_it != impl_->owned_names.end()) {
            for (const auto& name : names_it->second) {
                impl_->published_names.erase(name);
                retracted.push_back(name);
            }
            impl_->owned_names.erase(names_it);
        }
        // Erase the RPC state before the VM: sol handler handles reference
        // the VM's lua_State and must be destroyed while it is alive.
        impl_->service_rpc.erase(id);
        impl_->services.erase(id);
        impl_->service_order.erase(std::remove(impl_->service_order.begin(),
                                               impl_->service_order.end(), id),
                                   impl_->service_order.end());
        // Cap the tombstone set: dropping history only degrades the
        // service_dead vs service_not_found distinction for very old names.
        if (impl_->recently_exited.size() >= Impl::kRecentlyExitedLimit) {
            impl_->recently_exited.clear();
        }
        impl_->recently_exited.insert(id);

        // Drop any shield.httpd.* routes owned by this service: their
        // handlers belong to the VM being destroyed.
        impl_->runtime.remove_http_routes_for_service(id);

        // Tear down the service's CAF actor. For an own-actor exit, an exit
        // signal stops the actor once this handler unwinds; for a
        // foreign-thread exit the actor already received the
        // ServiceExitRequest and quits itself after on_exit — an exit signal
        // here could race and drop that request, so only the handle is
        // released.
        if (auto actor_it = impl_->service_actors.find(id);
            actor_it != impl_->service_actors.end()) {
            if (actor_it->second && exiting_from_own_actor) {
                caf::anon_send_exit(actor_it->second,
                                    caf::exit_reason::user_shutdown);
            }
            // Move the handle into the graveyard before erasing the entry:
            // releasing it here — possibly the last external reference —
            // would let this thread destroy the actor storage while the
            // actor's worker may still be inside cleanup() (see the
            // actors_to_wait comment above).
            impl_->retire_actor(std::move(actor_it->second));
            impl_->service_actors.erase(actor_it);
        }
    }
    for (const auto& name : retracted) {
        impl_->notify_name_change(name, "");
    }
    impl_->stop_and_wait_for_actors(actors_to_stop);
    // The drivers were joined above, but releasing the last reference on this
    // thread can still race the CAF worker's final unwind of a driver's
    // message loop — park the handles in the graveyard instead.
    impl_->retire_actors(std::move(actors_to_stop));
}

void LuaServiceManager::shutdown_all(std::string_view reason,
                                     int64_t stop_budget_ms) {
    impl_->stopping.store(true);
    const auto deadline = stop_budget_ms > 0
                              ? std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(stop_budget_ms)
                              : std::chrono::steady_clock::time_point::max();
    std::unordered_set<std::string> seen;
    std::vector<std::string> order;
    {
        std::shared_lock lock(impl_->registry_mutex);
        order = impl_->service_order;
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        bool exists = false;
        {
            std::shared_lock lock(impl_->registry_mutex);
            exists = impl_->services.contains(*it);
        }
        if (seen.insert(*it).second && exists) {
            if (std::chrono::steady_clock::now() < deadline) {
                exit(*it, reason);
            } else {
                // Graceful budget exhausted: skip on_exit and the per-service
                // teardown waits; just remove the registration and kill the
                // actor. A single stuck on_exit inside exit() is bounded by
                // the process-level shutdown watchdog, not this check.
                force_remove(*it, std::string(reason));
            }
        }
    }
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->service_order.clear();
    }
    // Unblock synchronous callers still waiting on their completion CV: the
    // runtime is stopping, so letting them ride out their full timeout would
    // only stall teardown. The destructor repeats this as a safety net for
    // calls that outlive shutdown_all.
    {
        std::unique_lock lock(impl_->registry_mutex);
        for (auto& [session, pending] : impl_->pending_sync_calls) {
            std::unique_lock lk(pending->mtx);
            pending->error = "runtime is stopping";
            pending->ok = false;
            pending->completed = true;
            pending->cv.notify_one();
        }
        impl_->pending_sync_calls.clear();
    }
}

void LuaServiceManager::force_remove(const std::string& id,
                                     const std::string& reason) {
    (void)reason;
    caf::actor actor;
    std::vector<std::string> retracted;
    {
        std::unique_lock lock(impl_->registry_mutex);
        if (auto names_it = impl_->owned_names.find(id);
            names_it != impl_->owned_names.end()) {
            for (const auto& name : names_it->second) {
                impl_->published_names.erase(name);
                retracted.push_back(name);
            }
            impl_->owned_names.erase(names_it);
        }
        // Erase the RPC state before the VM (sol handles reference the VM's
        // lua_State); see exit() for the mirrored ordering.
        impl_->service_rpc.erase(id);
        impl_->services.erase(id);
        impl_->service_order.erase(std::remove(impl_->service_order.begin(),
                                               impl_->service_order.end(), id),
                                   impl_->service_order.end());
        impl_->recently_exited.insert(id);
        impl_->runtime.remove_http_routes_for_service(id);
        if (auto actor_it = impl_->service_actors.find(id);
            actor_it != impl_->service_actors.end()) {
            // Move before erase: the entry's reference must not be released
            // on this thread while the actor's worker may still be running
            // (same graveyard policy as exit()).
            actor = std::move(actor_it->second);
            impl_->service_actors.erase(actor_it);
        }
    }
    cancel_forked_tasks_for_service(id);
    if (actor) {
        caf::anon_send_exit(actor, caf::exit_reason::user_shutdown);
        // anon_send_exit only queues the signal; this thread dropping the
        // last reference here could still race the actor's cleanup unwind.
        impl_->retire_actor(std::move(actor));
    }
    for (const auto& name : retracted) {
        impl_->notify_name_change(name, "");
    }
}

bool LuaServiceManager::enqueue_async_spawn(uint64_t session,
                                            std::string module,
                                            std::string opts_json) {
    if (impl_->stopping.load()) {
        return false;
    }
    {
        std::lock_guard lock(impl_->spawn_mutex);
        if (impl_->spawn_stop) {
            return false;
        }
        impl_->spawn_queue.push_back(
            Impl::SpawnJob{session, std::move(module), std::move(opts_json)});
    }
    impl_->spawn_cv.notify_one();
    return true;
}

void LuaServiceManager::spawn_worker_loop() {
    std::unique_lock lock(impl_->spawn_mutex);
    while (true) {
        impl_->spawn_cv.wait(lock, [this] {
            return impl_->spawn_stop || !impl_->spawn_queue.empty();
        });
        if (impl_->spawn_stop && impl_->spawn_queue.empty()) {
            return;
        }
        Impl::SpawnJob job = std::move(impl_->spawn_queue.front());
        impl_->spawn_queue.pop_front();
        lock.unlock();
        SpawnResult result = spawn(job.module, job.opts_json);
        finish_async_spawn(job.session, result);
        lock.lock();
    }
}

void LuaServiceManager::finish_async_spawn(uint64_t session,
                                           const SpawnResult& result) {
    bool caller_waiting = false;
    bool caller_alive = false;
    std::optional<caf::actor> caller_actor;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto pending_it = impl_->pending_calls.find(session);
        if (pending_it != impl_->pending_calls.end()) {
            caller_waiting = true;
            auto actor_it =
                impl_->service_actors.find(pending_it->second.caller_service);
            if (actor_it != impl_->service_actors.end() && actor_it->second) {
                caller_alive = true;
                caller_actor = actor_it->second;
            }
        }
    }

    if (!caller_waiting) {
        // The caller already resumed on timeout while init was still running.
        // Roll the successfully spawned child back so "spawn timeout" keeps
        // its documented meaning (init timeout -> service not started).
        if (result.success) {
            exit(result.service_id, "timeout");
        }
        return;
    }

    if (!caller_alive) {
        // Caller service is gone (typically runtime shutdown). Drop the wait
        // without touching its lua_State — the VM may already be destroyed.
        // The child stays running; teardown handles it during shutdown.
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.erase(session);
        return;
    }

    nlohmann::json values;
    bool ok = result.success;
    if (result.success) {
        values = nlohmann::json::array({result.service_id});
    } else {
        std::string code = "spawn_failed";
        if (result.error_message.find("timeout") != std::string::npos) {
            code = "spawn_timeout";
        } else if (result.error_message.find("on_init failed") !=
                   std::string::npos) {
            code = "init_failed";
        }
        values = nlohmann::json::array(
            {nlohmann::json::object({{"code", code},
                                     {"message", result.error_message},
                                     {"retryable", false}})});
    }

    // Route through the caller's actor mailbox (same channel as coroutine
    // call responses): resume_caller must run on the caller actor thread.
    CallResponseMessage response;
    response.session = session;
    response.ok = ok;
    response.values = std::move(values);
    caf::anon_send(*caller_actor, std::move(response));
}

void LuaServiceManager::panic_current(std::string_view reason) {
    const std::string service_id = current_service_id();
    if (service_id.empty()) {
        return;
    }
    std::shared_ptr<LuaVM> service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->services.find(service_id);
        if (it != impl_->services.end()) {
            service = it->second;
        }
    }
    if (service) {
        impl_->runtime.invoke_hook(service, "on_panic", std::string(reason),
                                   "explicit", "");
    }
    request_current_exit("panic");
}

std::string LuaServiceManager::current_service_id() const {
    return impl_->current_service_id();
}

std::shared_ptr<LuaVM> LuaServiceManager::service_vm(
    std::string_view service_id) const {
    std::shared_lock lock(impl_->registry_mutex);
    auto it = impl_->services.find(std::string(service_id));
    return it != impl_->services.end() ? it->second : nullptr;
}

std::string LuaServiceManager::current_sender_id() const {
    return impl_->current_sender_id();
}

std::string LuaServiceManager::current_trace_id() const {
    return impl_->current_trace_id();
}

int64_t LuaServiceManager::current_deadline_ms() const {
    return impl_->current_deadline_ms();
}

void LuaServiceManager::request_current_exit(std::string_view reason) {
    if (tls_dispatch_stack.empty()) {
        return;
    }
    auto& frame = tls_dispatch_stack.back();
    if (frame.in_exit) {
        return;
    }
    // Shared pending-exit store: the frame carrying this request may pop
    // before the dispatcher consumes it (a yielded on_init continues on a
    // resume frame of the caller actor thread), so the request must not
    // live only in thread-local state.
    impl_->request_exit_for(frame.service_id,
                            reason.empty() ? "normal" : std::string(reason));
}

bool LuaServiceManager::finish_pending_exit(const std::string& service_id) {
    // A request recorded while the service is still in spawn-init must be
    // driven by the spawn path after it publishes the service (exit() cannot
    // tear down an unpublished VM): leave it pending for spawn to consume.
    {
        std::shared_lock lock(impl_->registry_mutex);
        if (!impl_->services.contains(service_id)) {
            return false;
        }
    }
    std::string reason;
    if (!impl_->consume_exit_request(service_id, &reason)) {
        return false;
    }
    exit(service_id, reason);
    return true;
}

bool LuaServiceManager::is_in_exit() const {
    if (tls_dispatch_stack.empty()) {
        return false;
    }
    return tls_dispatch_stack.back().in_exit;
}

bool LuaServiceManager::spawn_init_in_progress() { return t_in_spawn_init; }

std::string LuaServiceManager::query_service(std::string_view name) const {
    std::shared_lock lock(impl_->registry_mutex);
    auto it = impl_->published_names.find(std::string(name));
    if (it == impl_->published_names.end()) {
        return "";
    }
    return it->second;
}

bool LuaServiceManager::register_name(std::string_view name,
                                      std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "register requires current service context";
        }
        return false;
    }
    const bool in_current_dispatch =
        !tls_dispatch_stack.empty() &&
        tls_dispatch_stack.back().service_id == owner;
    if (!Impl::valid_name(name)) {
        if (error) {
            *error = "invalid service name: " + std::string(name);
        }
        return false;
    }

    std::unique_lock lock(impl_->registry_mutex);
    if (!impl_->services.contains(owner) && !in_current_dispatch) {
        if (error) {
            *error = "current service is not running: " + owner;
        }
        return false;
    }
    if (auto existing = impl_->published_names.find(std::string(name));
        existing != impl_->published_names.end() && existing->second != owner) {
        if (error) {
            *error = "service name already exists: " + std::string(name);
        }
        return false;
    }

    impl_->published_names[std::string(name)] = owner;
    impl_->owned_names[owner].insert(std::string(name));
    lock.unlock();
    impl_->notify_name_change(std::string(name), owner);
    return true;
}

bool LuaServiceManager::unregister_name(std::string_view name,
                                        std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "unregister requires current service context";
        }
        return false;
    }

    std::unique_lock lock(impl_->registry_mutex);
    auto existing = impl_->published_names.find(std::string(name));
    if (existing == impl_->published_names.end()) {
        if (error) {
            *error = "service name not found: " + std::string(name);
        }
        return false;
    }
    if (existing->second != owner) {
        if (error) {
            *error = "service name is owned by another service: " +
                     std::string(name);
        }
        return false;
    }

    impl_->published_names.erase(existing);
    if (auto names_it = impl_->owned_names.find(owner);
        names_it != impl_->owned_names.end()) {
        names_it->second.erase(std::string(name));
    }
    lock.unlock();
    impl_->notify_name_change(std::string(name), "");
    return true;
}

void LuaServiceManager::set_name_change_notifier(
    std::function<void(const std::string&, const std::string&)> fn) {
    impl_->name_change_notifier = std::move(fn);
}

std::vector<std::string> LuaServiceManager::list_services() const {
    std::vector<std::string> services;
    {
        std::shared_lock lock(impl_->registry_mutex);
        services.reserve(impl_->published_names.size());
        for (const auto& [name, _] : impl_->published_names) {
            services.push_back(name);
        }
    }
    std::sort(services.begin(), services.end());
    return services;
}

uint64_t LuaServiceManager::enqueue_forked_task(std::string service_id,
                                                std::function<void()> task) {
    return enqueue_forked_task(std::move(service_id), std::move(task),
                               sol::function{});
}

uint64_t LuaServiceManager::enqueue_forked_task(std::string service_id,
                                                std::function<void()> task,
                                                sol::function raw_fn) {
    // Stored in pending_tasks for a lifetime that outlives the registering
    // coroutine: keep the reference's lua_State on the main thread (see
    // anchor_to_main_thread).
    raw_fn = anchor_to_main_thread(std::move(raw_fn));
    if (service_id.empty()) {
        // Hostless callers (e.g. the ops HTTP endpoints) want the task to run
        // on any managed dispatch thread. Borrow any live service actor so
        // the task is not silently dropped.
        std::shared_lock lock(impl_->registry_mutex);
        if (impl_->service_actors.empty()) {
            return 0;
        }
        service_id = impl_->service_actors.begin()->first;
    }

    const uint64_t id = impl_->next_task_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(impl_->task_mutex);
        impl_->pending_tasks.push_back(
            {id, service_id, std::move(task), std::move(raw_fn)});
        impl_->tasks_by_service[service_id].insert(id);
    }

    // Route the fork to the owning service actor. The actor stashes every
    // message until on_init completes (see spawn), so a fork scheduled during
    // on_init runs serially after init — no Lua VM race. The callback itself
    // is looked up by id in pending_tasks, so the sol::function never crosses
    // the CAF message boundary.
    std::shared_lock lock(impl_->registry_mutex);
    auto it = impl_->service_actors.find(service_id);
    if (it != impl_->service_actors.end()) {
        caf::anon_send(it->second, fork_task_atom_v, id);
        return id;
    }

    // Service actor not found: roll back the enqueue.
    {
        std::lock_guard<std::mutex> lock(impl_->task_mutex);
        auto by_service_it = impl_->tasks_by_service.find(service_id);
        if (by_service_it != impl_->tasks_by_service.end()) {
            by_service_it->second.erase(id);
            if (by_service_it->second.empty()) {
                impl_->tasks_by_service.erase(by_service_it);
            }
        }
        impl_->pending_tasks.erase(
            std::remove_if(
                impl_->pending_tasks.begin(), impl_->pending_tasks.end(),
                [id](const Impl::ForkedTask& t) { return t.id == id; }),
            impl_->pending_tasks.end());
    }
    return 0;
}

void LuaServiceManager::cancel_forked_tasks_for_service(
    const std::string& service_id) {
    std::unordered_set<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(impl_->task_mutex);
        auto it = impl_->tasks_by_service.find(service_id);
        if (it == impl_->tasks_by_service.end()) {
            return;
        }
        ids = it->second;
        impl_->tasks_by_service.erase(it);
        impl_->pending_tasks.erase(
            std::remove_if(impl_->pending_tasks.begin(),
                           impl_->pending_tasks.end(),
                           [&ids](const Impl::ForkedTask& t) {
                               return ids.count(t.id) > 0;
                           }),
            impl_->pending_tasks.end());
    }
}

size_t LuaServiceManager::pending_task_count(
    const std::string& service_id) const {
    std::lock_guard<std::mutex> lock(impl_->task_mutex);
    auto it = impl_->tasks_by_service.find(service_id);
    if (it == impl_->tasks_by_service.end()) {
        return 0;
    }
    return it->second.size();
}

size_t LuaServiceManager::pending_task_count_total() const {
    std::lock_guard<std::mutex> lock(impl_->task_mutex);
    return impl_->pending_tasks.size();
}

caf::actor_system& LuaServiceManager::actor_system() const {
    return impl_->system;
}

void LuaServiceManager::register_gateway_actor(std::string gateway_name,
                                               caf::actor actor) {
    std::unique_lock lock(impl_->registry_mutex);
    impl_->gateway_actors[std::move(gateway_name)] = std::move(actor);
}

caf::actor LuaServiceManager::gateway_actor(
    std::string_view gateway_name) const {
    std::shared_lock lock(impl_->registry_mutex);
    auto it = impl_->gateway_actors.find(std::string(gateway_name));
    return it != impl_->gateway_actors.end() ? it->second : caf::actor{};
}

// Push a JSON value onto a raw lua_State using the C API (avoids sol2
// stack-residue quirks when targeting a specific coroutine thread).
static void push_json_to_stack(lua_State* L, const nlohmann::json& v) {
    if (v.is_null()) {
        lua_pushnil(L);
    } else if (v.is_boolean()) {
        lua_pushboolean(L, v.get<bool>() ? 1 : 0);
    } else if (v.is_number_integer()) {
        lua_pushinteger(L, static_cast<lua_Integer>(v.get<std::int64_t>()));
    } else if (v.is_number_unsigned()) {
        lua_pushinteger(L, static_cast<lua_Integer>(v.get<std::uint64_t>()));
    } else if (v.is_number_float()) {
        lua_pushnumber(L, v.get<double>());
    } else if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
    } else if (v.is_array()) {
        lua_createtable(L, static_cast<int>(v.size()), 0);
        int i = 1;
        for (const auto& el : v) {
            push_json_to_stack(L, el);
            lua_rawseti(L, -2, i++);
        }
    } else if (v.is_object()) {
        // A trusted client-identity marker materializes as the read-only
        // ClientContext userdata instead of a plain table (same rule as
        // json_to_lua in lua_api.cpp). The global helper is registered into
        // every service VM by register_client_identity_api.
        if (auto ctx = ClientContextData::from_json(v)) {
            lua_getglobal(L, "__shield_make_client_context");
            lua_pushinteger(L, static_cast<lua_Integer>(ctx->session_id));
            lua_pushinteger(L, static_cast<lua_Integer>(ctx->session_epoch));
            lua_pushlstring(L, ctx->player_id.data(), ctx->player_id.size());
            lua_pushlstring(L, ctx->gateway_address.data(),
                            ctx->gateway_address.size());
            lua_pushlstring(L, ctx->protocol_profile_id.data(),
                            ctx->protocol_profile_id.size());
            lua_call(L, 5, 1);
            return;
        }
        lua_createtable(L, 0, static_cast<int>(v.size()));
        for (auto it = v.begin(); it != v.end(); ++it) {
            lua_pushlstring(L, it.key().data(), it.key().size());
            push_json_to_stack(L, it.value());
            lua_rawset(L, -3);
        }
    } else {
        lua_pushnil(L);
    }
}

uint64_t LuaServiceManager::suspend_for_call(lua_State* caller_co,
                                             int32_t timeout_ms) {
    const uint64_t session = impl_->next_call_session.fetch_add(1);
    Impl::PendingCall pc;
    pc.session = session;
    pc.caller_co = caller_co;
    // Arm the yield handshake: until the driving thread observes LUA_YIELD,
    // resume_caller must not touch the coroutine (see CallYieldSync).
    pc.yield_sync = std::make_shared<Impl::CallYieldSync>();
    // Anchor the caller coroutine against GC while it is suspended.
    lua_pushthread(caller_co);
    pc.caller_anchor = luaL_ref(caller_co, LUA_REGISTRYINDEX);
    const auto now = std::chrono::steady_clock::now();
    pc.deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count() +
                     (timeout_ms > 0 ? timeout_ms : 5000);
    pc.caller_service = current_service_id();
    const std::string service = pc.caller_service;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.emplace(session, std::move(pc));
    }

    // Step 2c: drive the call timeout via a CAF delayed event.
    if (!service.empty()) {
        const int32_t effective = timeout_ms > 0 ? timeout_ms : 5000;
        schedule_actor_call_timeout(effective, service, session);
    }
    return session;
}

void LuaServiceManager::set_handler_call_session(lua_State* co,
                                                 uint64_t session) {
    if (session != 0 && co != nullptr) {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->handler_call_session[co] = session;
    }
}

void LuaServiceManager::on_handler_completed(
    lua_State* co, const nlohmann::json& return_values) {
    if (co == nullptr) {
        return;
    }
    uint64_t session = 0;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->handler_call_session.find(co);
        if (it == impl_->handler_call_session.end()) {
            return;
        }
        session = it->second;
        impl_->handler_call_session.erase(it);
    }
    complete_call(session, true, return_values);
}

void LuaServiceManager::on_handler_failed(lua_State* co,
                                          const std::string& error_message) {
    if (co == nullptr) {
        return;
    }
    uint64_t session = 0;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->handler_call_session.find(co);
        if (it == impl_->handler_call_session.end()) {
            return;
        }
        session = it->second;
        impl_->handler_call_session.erase(it);
    }
    complete_call(session, false, nlohmann::json::array({error_message}));
}

namespace {

std::string call_error_message(const nlohmann::json& values) {
    if (values.is_array() && !values.empty()) {
        const auto& first = values.front();
        if (first.is_string()) {
            return first.get<std::string>();
        }
        if (first.is_object() && first.contains("message") &&
            first["message"].is_string()) {
            return first["message"].get<std::string>();
        }
        return first.dump();
    }
    if (values.is_string()) {
        return values.get<std::string>();
    }
    if (values.is_object() && values.contains("message") &&
        values["message"].is_string()) {
        return values["message"].get<std::string>();
    }
    return "call failed";
}

// Slack added to a proxied session's deadline on top of the caller's
// remaining timeout: the remote caller's own timeout is authoritative, this
// only bounds how long the entry may outlive a vanished caller.
constexpr int64_t kProxiedCallSlackMs = 30000;

}  // namespace

void LuaServiceManager::complete_call(uint64_t session, bool ok,
                                      const nlohmann::json& values) {
    // Whichever path completes first wins: cancel the expiry driver so a
    // late timeout cannot fire against a session that is already finished.
    cancel_actor_call_timeout(session);
    // External sync calls do not have a caller coroutine. Complete them
    // directly after the callee has fully unwound.
    {
        std::shared_ptr<Impl::PendingSyncCall> pending;
        {
            std::unique_lock lock(impl_->registry_mutex);
            auto sync_it = impl_->pending_sync_calls.find(session);
            if (sync_it != impl_->pending_sync_calls.end()) {
                pending = sync_it->second;
                impl_->pending_sync_calls.erase(sync_it);
            }
        }
        if (pending) {
            {
                std::unique_lock lk(pending->mtx);
                pending->ok = ok;
                if (ok) {
                    pending->values = values;
                } else {
                    pending->error = call_error_message(values);
                }
                pending->completed = true;
            }
            pending->cv.notify_one();
            return;
        }
    }

    // Proxied (remotely originated) calls have no local caller: route the
    // outcome through the hook so the transport replies to the source node.
    if (finish_proxied_call(session, ok, values)) {
        return;
    }

    std::optional<caf::actor> caller_actor;
    std::string caller_service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto pending_it = impl_->pending_calls.find(session);
        if (pending_it == impl_->pending_calls.end()) {
            return;
        }
        caller_service = pending_it->second.caller_service;
        auto actor_it = impl_->service_actors.find(caller_service);
        if (actor_it != impl_->service_actors.end()) {
            caller_actor = actor_it->second;
        }
    }
    if (!caller_actor) {
        // The caller's actor is gone (exited): nothing may resume the
        // suspended coroutine — resume_caller must run on the caller actor
        // thread, and an inline resume here would drive the caller's Lua VM
        // from a foreign thread. Drop the pending entry; the coroutine (and
        // its anchor) dies with the caller's VM.
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.erase(session);
        return;
    }

    CallResponseMessage response;
    response.session = session;
    response.ok = ok;
    response.values = values;
    caf::anon_send(*caller_actor, std::move(response));
}

uint64_t LuaServiceManager::begin_proxied_call(int32_t timeout_ms) {
    const uint64_t session = impl_->next_call_session.fetch_add(1);
    Impl::PendingCall pc;
    pc.session = session;
    pc.caller_co = nullptr;  // completion routes to the hook, not a coroutine
    pc.proxied = true;
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    pc.deadline_ms = now_ms + std::max(timeout_ms, 5000) + kProxiedCallSlackMs;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.emplace(session, std::move(pc));
    }
    return session;
}

void LuaServiceManager::abandon_proxied_call(uint64_t session) {
    std::unique_lock lock(impl_->registry_mutex);
    auto it = impl_->pending_calls.find(session);
    if (it != impl_->pending_calls.end() && it->second.proxied) {
        impl_->pending_calls.erase(it);
    }
}

void LuaServiceManager::set_proxied_call_hook(
    std::function<void(uint64_t, bool, const nlohmann::json&)> hook) {
    impl_->proxied_call_hook = std::move(hook);
}

bool LuaServiceManager::finish_proxied_call(uint64_t session, bool ok,
                                            const nlohmann::json& values) {
    std::function<void(uint64_t, bool, const nlohmann::json&)> hook;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->pending_calls.find(session);
        if (it == impl_->pending_calls.end() || !it->second.proxied) {
            return false;
        }
        impl_->pending_calls.erase(it);
        hook = impl_->proxied_call_hook;
    }
    // Cancel the expiry driver (if armed); unknown sessions no-op.
    cancel_actor_call_timeout(session);
    if (hook) {
        hook(session, ok, values);
    }
    return true;
}

CallResult LuaServiceManager::call_with_session(
    const std::function<bool(uint64_t, std::string&)>& initiate,
    int32_t timeout_ms) {
    if (impl_->stopping.load()) {
        return CallResult::error("runtime is stopping");
    }

    const uint64_t session = impl_->next_call_session.fetch_add(1);
    auto pending = std::make_shared<Impl::PendingSyncCall>();
    pending->session = session;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls[session] = pending;
    }

    // Hand the session to the caller-provided starter (e.g. a remote
    // envelope send). A synchronous failure means complete_call will never
    // fire for this session: fail the call on the spot.
    std::string dispatch_error;
    if (!initiate || !initiate(session, dispatch_error)) {
        {
            std::unique_lock lock(impl_->registry_mutex);
            impl_->pending_sync_calls.erase(session);
        }
        return CallResult::error(dispatch_error.empty() ? "call dispatch failed"
                                                        : dispatch_error);
    }

    // Expiry is driven by the CAF delayed driver (it completes the call with
    // a timeout error); fall back to a timed wait if the driver was not
    // armed.
    const int32_t effective_timeout = timeout_ms > 0 ? timeout_ms : 5000;
    const bool driver_armed =
        schedule_external_call_timeout(effective_timeout, session);
    bool completed = false;
    {
        std::unique_lock lk(pending->mtx);
        if (driver_armed) {
            pending->cv.wait(lk, [&] { return pending->completed; });
        } else {
            completed = pending->cv.wait_for(
                lk, std::chrono::milliseconds(effective_timeout),
                [&] { return pending->completed; });
        }
        completed = completed || pending->completed;
    }

    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls.erase(session);
    }

    if (!completed) {
        return CallResult::error(
            "call timeout (actor dispatch exceeded limit)");
    }
    if (pending->ok) {
        return CallResult::ok(std::move(pending->values));
    }
    return CallResult::error(std::move(pending->error));
}

uint64_t LuaServiceManager::dispatch_remote_call(std::string_view service_id,
                                                 std::string_view method,
                                                 const nlohmann::json& args,
                                                 int32_t timeout_ms,
                                                 std::string* error) {
    const uint64_t session = begin_proxied_call(timeout_ms);
    if (!dispatch_proxied_call(session, service_id, method, args, error)) {
        abandon_proxied_call(session);
        return 0;
    }
    return session;
}

bool LuaServiceManager::dispatch_proxied_call(uint64_t session,
                                              std::string_view service_id,
                                              std::string_view method,
                                              const nlohmann::json& args,
                                              std::string* error) {
    if (!send_call_request(service_id, method, args, session, error)) {
        return false;
    }
    // Arm the expiry driver from the session's deadline (begin_proxied_call
    // derived it from the envelope's caller timeout + slack).
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    int64_t delay_ms = 1;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->pending_calls.find(session);
        if (it != impl_->pending_calls.end()) {
            delay_ms = it->second.deadline_ms - now_ms;
        }
    }
    schedule_proxied_call_timeout(
        session, static_cast<int32_t>(delay_ms > 0 ? delay_ms : 1));
    return true;
}

void LuaServiceManager::schedule_proxied_call_timeout(uint64_t session,
                                                      int32_t timeout_ms) {
    if (timeout_ms <= 0) {
        return;
    }
    try {
        auto driver = impl_->system.spawn(
            [manager = this, session,
             timeout_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(timeout_ms),
                                   caf::tick_atom_v);
                return caf::behavior{[=](caf::tick_atom) {
                    nlohmann::json timeout_err = nlohmann::json::array(
                        {nlohmann::json::object({{"code", "timeout"},
                                                 {"message", "call timeout"},
                                                 {"retryable", true}})});
                    manager->finish_proxied_call(session, false, timeout_err);
                    self->quit();
                }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_call_timeouts[session] = std::move(driver);
    } catch (const std::exception& e) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(log, std::string("Failed to spawn proxied call "
                                          "timeout actor: ") +
                                  e.what());
        // Without the driver the entry still expires via its deadline in
        // check_call_timeouts scans; nothing else breaks.
    }
}

void LuaServiceManager::mark_call_yielded(lua_State* co) {
    if (co == nullptr) {
        return;
    }
    // The driving thread observed LUA_YIELD: every suspension this coroutine
    // registered is now safe to resume from any thread. There is at most one
    // unsatisfied suspension per coroutine (the wrappers yield serially), but
    // marking all matching entries is harmless — completed sessions are
    // already erased from pending_calls.
    std::shared_lock lock(impl_->registry_mutex);
    for (auto& [session, pc] : impl_->pending_calls) {
        if (pc.caller_co == co && pc.yield_sync != nullptr) {
            std::lock_guard sync_lock(pc.yield_sync->mtx);
            pc.yield_sync->yielded = true;
            pc.yield_sync->cv.notify_all();
        }
    }
}

void LuaServiceManager::resume_caller(uint64_t session, bool ok,
                                      const nlohmann::json& values) {
    Impl::PendingCall pc;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->pending_calls.find(session);
        if (it == impl_->pending_calls.end()) {
            return;
        }
        pc = std::move(it->second);
        impl_->pending_calls.erase(it);
    }

    // Cancel the CAF call-timeout driver (if any) now that the call has
    // completed normally. The timeout path itself erases the driver before
    // calling resume_caller, so this is a no-op for the timeout branch.
    cancel_actor_call_timeout(session);

    lua_State* caller_co = pc.caller_co;
    if (caller_co == nullptr) {
        return;
    }

    // Yield handshake: if the caller has not reached its coroutine.yield()
    // yet, it is still running on the thread that registered the suspension;
    // resuming it here would fail ("cannot resume running coroutine") and the
    // completion would be lost with no recovery source. Wait for the driving
    // thread to mark the yield. The wait is bounded: a wrapper that never
    // yields (protocol violation) must not wedge an actor thread forever —
    // the resume below then fails into the existing error path.
    if (pc.yield_sync != nullptr) {
        std::unique_lock sync_lock(pc.yield_sync->mtx);
        if (!pc.yield_sync->yielded) {
            pc.yield_sync->cv.wait_for(sync_lock, std::chrono::seconds(5),
                                       [&] { return pc.yield_sync->yielded; });
        }
    }

    // Build the resume payload: (ok, values...). The caller's shield.call
    // wrapper unpacks these via coroutine.yield()'s return values.
    //
    // Re-establish the caller's dispatch context for the continuation: the
    // resume runs on the caller actor thread, whose thread-local dispatch
    // stack does not carry the original frame (an on_init coroutine, for
    // example, is first driven by the spawning thread). Without this frame
    // every suspend_for_call issued after the first resume records an empty
    // caller_service and the next completion cannot route back.
    Impl::DispatchScope scope(*impl_, pc.caller_service, "", false);
    lua_pushboolean(caller_co, ok ? 1 : 0);
    int nargs = 1;
    if (values.is_array()) {
        for (const auto& v : values) {
            push_json_to_stack(caller_co, v);
            ++nargs;
        }
    } else if (!values.is_null()) {
        push_json_to_stack(caller_co, values);
        ++nargs;
    }
    int nres = 0;
    const int status = lua_resume(caller_co, nullptr, nargs, &nres);
    if (status == LUA_OK) {
        nlohmann::json returns = nlohmann::json::array();
        for (int i = 0; i < nres; ++i) {
            nlohmann::json item;
            sol::stack_object so(sol::state_view(caller_co), i + 1);
            lua_to_json(so, &item);
            returns.push_back(std::move(item));
        }
        lua_settop(caller_co, 0);
        if (pc.caller_anchor != LUA_NOREF) {
            luaL_unref(caller_co, LUA_REGISTRYINDEX, pc.caller_anchor);
        }
        on_handler_completed(caller_co, returns);
        finish_pending_exit(pc.caller_service);
        return;
    }
    if (status != LUA_YIELD) {
        std::string err = "call continuation error";
        if (lua_type(caller_co, -1) == LUA_TSTRING) {
            err = lua_tostring(caller_co, -1);
        }
        // Caller errored resuming; drop it after propagating to its upstream
        // caller if this handler was itself servicing a call.
        lua_settop(caller_co, 0);
        if (pc.caller_anchor != LUA_NOREF) {
            luaL_unref(caller_co, LUA_REGISTRYINDEX, pc.caller_anchor);
        }
        on_handler_failed(caller_co, err);
        finish_pending_exit(pc.caller_service);
        return;
    }
    // Release the anchor; the coroutine re-yielded and the API that yielded has
    // already re-anchored it for its next resume source.
    if (pc.caller_anchor != LUA_NOREF) {
        luaL_unref(caller_co, LUA_REGISTRYINDEX, pc.caller_anchor);
    }
    // The re-yield may correspond to a nested suspension (e.g. the handler
    // issued another shield.call): re-arm the handshake for its next resume
    // source.
    mark_call_yielded(caller_co);
}

int LuaServiceManager::check_call_timeouts(int64_t now_ms) {
    // Collect expired sessions first; we must not modify pending_calls while
    // iterating, and resume_caller erases from it.
    std::vector<uint64_t> expired;
    {
        std::shared_lock lock(impl_->registry_mutex);
        for (const auto& [session, pc] : impl_->pending_calls) {
            if (pc.deadline_ms <= now_ms) {
                expired.push_back(session);
            }
        }
    }

    nlohmann::json timeout_err = nlohmann::json::array(
        {nlohmann::json::object({{"code", "timeout"},
                                 {"message", "call timeout"},
                                 {"retryable", true}})});

    for (uint64_t session : expired) {
        // Proxied sessions have no coroutine to resume, but their expiry
        // must still reach the hook so the remote caller learns of the
        // timeout instead of hanging until its own deadline.
        if (finish_proxied_call(session, false, timeout_err)) {
            continue;
        }
        resume_caller(session, false, timeout_err);
    }
    return static_cast<int>(expired.size());
}

void LuaServiceManager::invoke_error_hook(const std::string& service_id,
                                          const std::string& error_type,
                                          const std::string& method_name,
                                          const std::string& error_message) {
    std::shared_ptr<LuaVM> service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->services.find(service_id);
        if (it != impl_->services.end()) {
            service = it->second;
        }
    }
    if (!service) {
        return;
    }

    // Increment error counter (read the count under the lock; the panic
    // threshold check below uses the local snapshot).
    int count = 0;
    {
        std::lock_guard lock(impl_->error_mutex);
        count = ++impl_->error_counts[service_id];
    }

    // Call on_error(err, context) if defined on the service table.
    impl_->runtime.invoke_hook(service, "on_error", error_message, error_type,
                               method_name);

    // Check panic threshold.
    if (count >= kDefaultMaxErrorsBeforePanic) {
        impl_->runtime.invoke_hook(service, "on_panic",
                                   "consecutive errors reached limit",
                                   error_type, method_name);
        // Exit the service after panic.
        exit(service_id, "panic");
    }
}

void LuaServiceManager::reset_error_count(const std::string& service_id) {
    std::lock_guard lock(impl_->error_mutex);
    impl_->error_counts.erase(service_id);
}

bool LuaServiceManager::exec_lua(const std::string& service_id,
                                 const std::string& code,
                                 nlohmann::json* result, std::string* error) {
    std::shared_ptr<LuaVM> service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->services.find(service_id);
        if (it != impl_->services.end()) {
            service = it->second;
        }
    }
    if (!service) {
        if (error) *error = "Service not found: " + service_id;
        return false;
    }
    return impl_->runtime.exec_lua(service, code, result, error);
}

void LuaServiceManager::attach_clock(std::shared_ptr<Clock> clock) {
    if (!clock) {
        return;  // reject null; keep existing clock
    }
    std::unique_lock lock(impl_->registry_mutex);
    impl_->clock_ = std::move(clock);
}

int64_t LuaServiceManager::clock_now_ms() const {
    return impl_->clock_now_ms();
}

int64_t LuaServiceManager::clock_now_seconds() const {
    return impl_->clock_now_seconds();
}

uint64_t LuaServiceManager::schedule_actor_timer_once(
    int64_t delay_ms, sol::function callback, const std::string& service_id) {
    if (!callback.valid()) {
        return 0;
    }
    // Stored for a lifetime that outlives the registering coroutine: keep the
    // reference's lua_State on the main thread (see anchor_to_main_thread).
    callback = anchor_to_main_thread(std::move(callback));
    std::shared_ptr<caf::actor> service_actor;
    uint64_t id = 0;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it == impl_->service_actors.end()) {
            return 0;
        }
        service_actor = std::make_shared<caf::actor>(it->second);
        id = impl_->next_actor_timer_id.fetch_add(1);
        impl_->actor_timers[id] = LuaServiceManager::Impl::ActorTimerState{
            .id = id,
            .interval_ms = delay_ms,
            .repeating = false,
            .service_id = service_id,
            .raw_callback = callback,
            .native_callback = {},
            .has_native_callback = false,
            .active = true,
            .driver = caf::actor{}};
    }

    try {
        auto driver = impl_->system.spawn(
            [svc = service_id, tid = id, target = *service_actor,
             delay_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(delay_ms),
                                   caf::tick_atom_v);
                return caf::behavior{[=](caf::tick_atom) {
                    caf::anon_send(target, timer_fire_atom_v, tid);
                    self->quit();
                }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers.end()) {
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
    } catch (const std::exception& e) {
        // Clean up the timer state if spawn fails
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_timers.erase(id);
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(
            log, std::string("Failed to spawn timer actor: ") + e.what());
        return 0;
    }
    return id;
}

uint64_t LuaServiceManager::schedule_actor_timer_once_fn(
    int64_t delay_ms, std::function<void()> callback,
    const std::string& service_id) {
    std::shared_ptr<caf::actor> service_actor;
    uint64_t id = 0;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it == impl_->service_actors.end()) {
            return 0;
        }
        service_actor = std::make_shared<caf::actor>(it->second);
        id = impl_->next_actor_timer_id.fetch_add(1);
        impl_->actor_timers[id] = LuaServiceManager::Impl::ActorTimerState{
            .id = id,
            .interval_ms = delay_ms,
            .repeating = false,
            .service_id = service_id,
            .raw_callback = sol::function{},
            .native_callback = std::move(callback),
            .has_native_callback = true,
            .active = true,
            .driver = caf::actor{}};
    }

    try {
        auto driver = impl_->system.spawn(
            [svc = service_id, tid = id, target = *service_actor,
             delay_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(delay_ms),
                                   caf::tick_atom_v);
                return caf::behavior{[=](caf::tick_atom) {
                    caf::anon_send(target, timer_fire_atom_v, tid);
                    self->quit();
                }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers.end()) {
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
    } catch (const std::exception& e) {
        // Clean up the timer state if spawn fails
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_timers.erase(id);
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(
            log, std::string("Failed to spawn timer actor: ") + e.what());
        return 0;
    }
    return id;
}

uint64_t LuaServiceManager::schedule_actor_timer_fixed_delay(
    int64_t interval_ms, sol::function callback,
    const std::string& service_id) {
    if (!callback.valid()) {
        return 0;
    }
    // Stored for a lifetime that outlives the registering coroutine: keep the
    // reference's lua_State on the main thread (see anchor_to_main_thread).
    callback = anchor_to_main_thread(std::move(callback));
    std::shared_ptr<caf::actor> service_actor;
    uint64_t id = 0;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it == impl_->service_actors.end()) {
            return 0;
        }
        service_actor = std::make_shared<caf::actor>(it->second);
        id = impl_->next_actor_timer_id.fetch_add(1);
        impl_->actor_timers[id] = LuaServiceManager::Impl::ActorTimerState{
            .id = id,
            .interval_ms = interval_ms,
            .repeating = true,
            .service_id = service_id,
            .raw_callback = callback,
            .native_callback = {},
            .has_native_callback = false,
            .active = true,
            .driver = caf::actor{}};
    }

    try {
        auto driver = impl_->system.spawn(
            [svc = service_id, tid = id, target = *service_actor,
             interval_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(interval_ms),
                                   caf::tick_atom_v);
                return caf::behavior{[=](caf::tick_atom) {
                    caf::anon_send(target, timer_fire_atom_v, tid);
                    self->delayed_send(self,
                                       std::chrono::milliseconds(interval_ms),
                                       caf::tick_atom_v);
                }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers.end()) {
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
    } catch (const std::exception& e) {
        // Clean up the timer state if spawn fails
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_timers.erase(id);
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(
            log, std::string("Failed to spawn timer actor: ") + e.what());
        return 0;
    }
    return id;
}

bool LuaServiceManager::cancel_actor_timer(uint64_t id) {
    std::vector<caf::actor> actors_to_stop;
    const bool cancelled =
        impl_->collect_actor_timer_for_cancel(id, &actors_to_stop);
    impl_->stop_and_wait_for_actors(actors_to_stop);
    // Same graveyard policy as exit(): joined above, released later.
    impl_->retire_actors(std::move(actors_to_stop));
    return cancelled;
}

size_t LuaServiceManager::active_actor_timer_count() const {
    std::shared_lock lock(impl_->registry_mutex);
    return impl_->actor_timers.size();
}

bool LuaServiceManager::schedule_external_call_timeout(int32_t timeout_ms,
                                                       uint64_t session) {
    if (timeout_ms <= 0) {
        return false;
    }
    try {
        auto driver = impl_->system.spawn(
            [manager = this, session,
             timeout_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(timeout_ms),
                                   caf::tick_atom_v);
                return caf::behavior{[=](caf::tick_atom) {
                    nlohmann::json timeout_err = nlohmann::json::array(
                        {nlohmann::json::object({{"code", "timeout"},
                                                 {"message", "call timeout"},
                                                 {"retryable", true}})});
                    manager->complete_call(session, false, timeout_err);
                    self->quit();
                }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_call_timeouts[session] = std::move(driver);
        return true;
    } catch (const std::exception& e) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(log, std::string("Failed to spawn external call "
                                          "timeout driver: ") +
                                  e.what());
        return false;
    }
}

uint64_t LuaServiceManager::schedule_actor_call_timeout(
    int32_t timeout_ms, const std::string& service_id, uint64_t session) {
    if (timeout_ms <= 0) {
        return session;
    }
    std::shared_ptr<caf::actor> service_actor;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it == impl_->service_actors.end()) {
            return session;
        }
        service_actor = std::make_shared<caf::actor>(it->second);
    }

    try {
        auto driver = impl_->system.spawn(
            [manager = this, session, target = *service_actor,
             timeout_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(timeout_ms),
                                   caf::tick_atom_v);
                return caf::behavior{[=](caf::tick_atom) {
                    caf::anon_send(target, call_timeout_atom_v, session);
                    self->quit();
                }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_call_timeouts[session] = std::move(driver);
    } catch (const std::exception& e) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(
            log,
            std::string("Failed to spawn call timeout actor: ") + e.what());
        // Return session anyway - timeout just won't fire, but call can still
        // complete
    }
    return session;
}

bool LuaServiceManager::cancel_actor_call_timeout(uint64_t session) {
    caf::actor cancelled_driver;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_call_timeouts.find(session);
        if (it == impl_->actor_call_timeouts.end()) {
            return false;
        }
        if (it->second) {
            caf::anon_send_exit(it->second, caf::exit_reason::user_shutdown);
            // The entry dies below while the driver may still be running its
            // (or a still-armed) tick; move the handle out so the erase never
            // releases a reference on this thread, and park it in the
            // graveyard — the Impl destructor joins it there.
            cancelled_driver = std::move(it->second);
        }
        impl_->actor_call_timeouts.erase(it);
    }
    impl_->retire_actor(std::move(cancelled_driver));
    return true;
}

}  // namespace shield::lua
