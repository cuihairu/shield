// [SHIELD_LUA] LuaServiceManager::Impl - private implementation state
//
// The pimpl payload of LuaServiceManager: registry, dispatch bookkeeping,
// timers, fork queue, inspect/profile state and the CAF actor plumbing.
// Internal to src/lua/ - not a public contract. Data declarations only;
// every member function is declared here and defined out-of-line in the
// lua_service_*.cpp translation units (single instantiation per function,
// which keeps gcov branch accounting per TU exact).
#pragma once

#include <atomic>
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/profile_sampler.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::lua {

// True on the thread currently running a spawn's on_init (see
// lua_service.cpp, which owns the definition, for the full contract).
extern thread_local bool t_in_spawn_init;

// One frame per nested dispatch (spawn on_init, message handlers, call
// resumes). Shared by the lua_service_*.cpp translation units through this
// declaration; the single thread_local definition lives in lua_service.cpp.
struct DispatchFrame {
    std::string service_id;
    std::string sender_id;
    std::string trace_id;
    int64_t deadline_ms = 0;
    bool in_exit = false;
    // Set by the spawn scope only: during on_init the service is not yet
    // in the registry, so current_service_vm() needs the frame's VM.
    std::shared_ptr<LuaVM> vm;
};

extern thread_local std::vector<DispatchFrame> tls_dispatch_stack;

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

    // VMs retired by the hung teardown (shutdown budget exhausted while
    // on_exit is still running). Declared BEFORE service_rpc so that, at
    // destruction, the sol handles in service_rpc unref against a still
    // alive lua_State — a hung VM must outlive every handle that points
    // into it, which is why these are never destroyed here (the leak is
    // bounded by the number of stuck services a process may accumulate).
    std::vector<std::shared_ptr<LuaVM>> hung_vms;
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
        std::unordered_map<std::uint32_t, shd::function> handlers;
    };
    std::unordered_map<std::string, ServiceRpcState> service_rpc;

    // Cumulative per-service traffic counters (requests/errors), one entry
    // per published incarnation: spawn inserts fresh, every teardown path
    // erases alongside services. Guarded by registry_mutex like the other
    // per-service maps; dispatch copies the shared_ptr out under the lock
    // and bumps the relaxed atomics lock-free on the service actor thread.
    // spawned_at is pinned at construction — i.e. the publish instant — so
    // the entry also carries the incarnation's uptime baseline.
    struct ServiceCounters {
        std::atomic<std::uint64_t> requests{0};
        std::atomic<std::uint64_t> errors{0};
        // Lua heap KB sampled at the last dispatch exit on the owning
        // thread (O(1) gc query). Relaxed: an observability gauge.
        std::atomic<std::uint64_t> memory_kb{0};
        std::chrono::steady_clock::time_point spawned_at{
            std::chrono::steady_clock::now()};
    };
    std::unordered_map<std::string, std::shared_ptr<ServiceCounters>>
        service_counters;

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
    void request_exit_for(const std::string& service_id, std::string reason);

    // Consumes a pending exit request for a service: returns true (exactly
    // once) with the requested reason, or false when none is pending.
    bool consume_exit_request(const std::string& service_id,
                              std::string* reason);

    mutable std::shared_mutex registry_mutex;

    // VM lookup for timer/fork dispatch: a published service wins; while a
    // spawn's on_init is still running the VM is only in init_vms.
    std::shared_ptr<LuaVM> find_dispatch_vm(const std::string& id);

    // Caller holds registry_mutex. Drops every live-coroutine entry owned
    // by `id`: the service is leaving the registry and its suspended
    // coroutines will never observe a terminal resume. Never touches the
    // lua_States themselves — teardown only retires bookkeeping. The GC
    // anchor refs (CoroutineMeta::anchor_ref) are intentionally NOT
    // released here: a hung service's lua_State must never be touched from
    // outside its actor, and every other path destroys the VM outright,
    // taking the registry (and the refs) with it.
    void drop_live_coroutines_locked(const std::string& id);

    // Settle an active /ops/profile session whose service is leaving:
    // fulfill the promise as abandoned (the caller's 2s wait ends with an
    // honest answer instead of a timeout), unpublish the sampler, and let
    // the state die when this critical section ends. The sampler dies
    // without an uninstall pass — in hung_service_teardown the VM is
    // parked (its lua_State is untouchable) and on the other paths the VM
    // dies with the hooks on it; a count hook on an undriven VM never
    // fires. Must hold the registry mutex. Unlike the drop_* helpers this
    // does not run on the owner thread, so it never calls uninstall().
    void abandon_profile_session_locked(const std::string& id);

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
    // set by drain_all, checked by spawn paths (no spawn during the
    // on_shutdown drain); deliberately NOT checked by send/call so drain
    // hooks can flush state through shield.call
    std::atomic<bool> draining{false};

    // Observer for name publication changes (set once at bootstrap, before
    // any spawn; read without the registry lock, invoked outside of it).
    std::function<void(const std::string&, const std::string&)>
        name_change_notifier;

    // Forward one committed name change to the notifier (never called while
    // holding registry_mutex).
    void notify_name_change(const std::string& name,
                            const std::string& service_id);

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
        shd::function raw_fn;  // original Lua function, for coroutine wrapping
    };
    std::atomic<uint64_t> next_task_id{1};
    std::vector<ForkedTask> pending_tasks;
    std::unordered_map<std::string, std::unordered_set<uint64_t>>
        tasks_by_service;
    std::mutex task_mutex;
    // Execute-phase drain signal: pending_tasks hits zero at dequeue time,
    // while the dequeued task's body (which touches the registering VM from
    // the actor thread) is still running. Bumped inside the same task_mutex
    // critical section as the dequeue erase, dropped after the body —
    // waiting on both counters being zero leaves no dequeue/execute gap.
    std::atomic<std::size_t> active_fork_tasks{0};

    // Coroutine call correlation. A call from a handler yields the caller's
    // coroutine; the callee runs and, on completion, resumes the caller with
    // the callee's return values.
    //
    // Driving-phase guard: every coroutine resume source registers the
    // thread state it is about to drive in driving_cos (registry-locked)
    // for the whole lua_resume span. resume_caller consults the set instead
    // of peeking at the coroutine's Lua state — reading lua_status /
    // lua_getstack of a thread another OS thread is actively driving is a
    // data race (MSVC release builds crashed on exactly that in
    // CallErrorCodesAreStable), while the set read is mutex-ordered. A
    // response arriving while the caller is registered (the yield window:
    // suspend_for_call runs inside the still-running coroutine) is
    // re-enqueued through the caller actor; everything else resumes
    // directly.
    std::unordered_set<lua_State*> driving_cos;
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
        // Yield-window requeue counter (resume_caller re-enqueues the
        // response while the caller coroutine is still running on its
        // driving thread; the cap bounds the loop for a caller that never
        // yields — it matches the historical handshake wait's drop).
        int requeues = 0;
        // Rejected-resume requeue counter (resume_suspended_caller's
        // concurrent-completion guard). Deliberately independent of
        // requeues: the driving-phase spin can legitimately burn its whole
        // cap inside one init-drive span (the span contains CAF scheduler
        // operations that are millisecond-scale under load, while one
        // mailbox self-loop is microsecond-scale), and the guard must stay
        // armed when that spin trips its cap and falls through.
        int resume_retries = 0;
        // Phase B slow-call metering: the begin stamp (steady ms) taken in
        // suspend_for_call when the process-wide gate was armed, else 0 —
        // the completion path treats 0 as "never metered". callee rides
        // along so resume_caller needs no second lookup at completion.
        uint64_t begin_ms = 0;
        std::string callee;
        // DB-async completion claim (lua_resume_session): set by the first
        // worker completion so a second one (double resume, or a racing
        // worker on the same session) is rejected instead of double-resuming
        // the parked coroutine. shield.call completions never touch it —
        // their exactly-once property comes from resume_caller's erase.
        bool async_claimed = false;
    };
    std::atomic<uint64_t> next_call_session{1};
    std::unordered_map<uint64_t, PendingCall>
        pending_calls;  // session -> caller wait
    std::unordered_map<lua_State*, uint64_t>
        handler_call_session;  // callee co -> session

    // Live handler coroutines keyed by their lua_State, valued by the
    // owning service id. Same registry-lock domain as the other per-service
    // registry maps: every coroutine resume source erases on terminal
    // states and service teardown drops the service's remaining entries
    // (a suspended coroutine whose service exits never resumes).
    std::unordered_map<lua_State*, std::string> live_coroutines;

    // Per-coroutine resume bookkeeping, key domain identical to
    // live_coroutines (inserted by note_coroutine_started, erased at the
    // same points). origin is the dispatch kind that first drove the
    // coroutine (invoke_coroutine's error_type); last_resume records the
    // most recent C++ resume source ("dispatch" initially, then
    // "call-response"/"call-timeout"). Lua-driven resumes (wrap) have no
    // C++ observation point and leave the bookkeeping at its last value.
    struct CoroutineMeta {
        std::string origin;
        std::uint64_t resumes = 0;
        std::string last_resume;
        std::int64_t last_resume_ms = 0;
        // Registry ref anchoring the coroutine's thread against the GC
        // while it is bookkept (see note_coroutine_started in the header).
        int anchor_ref = LUA_NOREF;
    };
    std::unordered_map<lua_State*, CoroutineMeta> coroutine_meta;

    // L2 inspect snapshots (lua.snapshot / lua.diff): registry-locked
    // copies of a service's L1 gauges. Bounded ring per service; entries
    // die with the incarnation (teardown drops the deque).
    struct InspectSnapshot {
        // Owner-thread object-graph summary, captured only when the caller
        // asks (with_refs): flat counts mirror the walk_module_refs JSON so
        // a diff can produce per-field deltas, and top_tables keeps the
        // walk's path/entries pairs in report order.
        struct RefsSummary {
            std::int64_t nodes_visited = 0;
            bool truncated = false;
            std::int64_t tables = 0;
            std::int64_t functions = 0;
            std::int64_t userdata = 0;
            std::int64_t coroutines = 0;
            std::int64_t strings = 0;
            std::int64_t string_bytes = 0;
            struct TopTable {
                std::string path;
                std::int64_t entries = 0;
            };
            std::vector<TopTable> top_tables;
        };
        std::string name;
        std::int64_t wall_ms = 0;
        std::uint64_t requests = 0;
        std::uint64_t errors = 0;
        std::uint64_t memory_kb = 0;
        std::uint64_t pending_calls = 0;
        std::size_t pending_tasks = 0;
        std::size_t coroutines = 0;
        std::size_t timers = 0;
        double uptime_seconds = 0.0;
        // Absent unless the capture asked for refs — ring entries stay
        // cheap by default. refs_error records an owner-busy timeout (the
        // gauges are still captured; the graph is simply not sampled).
        std::optional<RefsSummary> refs;
        std::string refs_error;
    };
    static constexpr std::size_t kInspectSnapshotsPerService = 8;
    std::map<std::string, std::deque<InspectSnapshot>> inspect_snapshots;
    std::uint64_t next_snapshot_seq = 0;

    // /ops/profile sampling session: at most one process-wide (the
    // session's service_id gates start/stop matching). Guarded by the
    // registry mutex like every other observability field. `sampler` is
    // created by the install fork task on the owner thread and destroyed
    // by the uninstall task (or the exit cleanup) — the unique_ptr is
    // only ever dereferenced on the owning actor thread, while the
    // optional's presence bit is registry-lock data.
    struct ProfileSessionState {
        std::string service_id;
        std::shared_ptr<ProfileSession> session;
        // Shared, not unique: the global slot carries the same shared_ptr,
        // and readers (the hook, sweep_active) copy it, so the sampler and
        // its session outlive this state until the last reader is done.
        std::shared_ptr<ProfileSampler> sampler;
        std::shared_ptr<std::promise<nlohmann::json>> done;
        caf::actor duration_driver;
        std::chrono::steady_clock::time_point started_at;
    };
    std::optional<ProfileSessionState> profile_session;

    // Read-only JSON form of the refs summary (null when not captured).
    static nlohmann::json refs_summary_to_json(
        const InspectSnapshot::RefsSummary& r);

    // Read-only JSON form of a stored L2 snapshot (capture echoes it back;
    // diff embeds both ends plus the delta).
    static nlohmann::json snapshot_to_json(const InspectSnapshot& s);

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
        // Steady-clock due time of the next fire (set at registration,
        // advanced at every repeating fire) — the timer_inspect projection
        // reports the nearest one. Bookkeeping only; the CAF driver actor
        // owns the actual schedule.
        int64_t next_fire_ms = 0;
        std::string service_id;
        shd::function raw_callback;
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

    int64_t clock_now_ms() const;

    int64_t clock_now_seconds() const;

    // Dispatch a CAF-native ServiceMessage. Converts the typed fields into an
    // internal DispatchMessage and routes to the existing dispatch_message
    // path.
    void dispatch_service_message(class LuaServiceManager* manager,
                                  const std::string& id,
                                  const ServiceMessage& msg);

    void dispatch_call_response(class LuaServiceManager* manager,
                                const CallResponseMessage& msg);

    // Route a validated client-to-server payload to the target VM's compiled
    // RPC handler. Unlike ServiceMessage dispatch there is no method-name
    // lookup: the spawn-time RPC table owns route_id -> handler, so an
    // unknown route here is a startup-contract violation and is dropped with
    // a warning (never queued or retried). The request value follows the
    // descriptor contract: a pipeline-decoded canonical message wins, else a
    // JSON codec decodes body_bytes, else the raw bytes travel as a string.
    void dispatch_client_ingress(class LuaServiceManager* manager,
                                 const std::string& id,
                                 const ClientIngress& msg);

    // Session lifecycle notification from the gateway: map the control kind
    // to its Lua convention handler and reuse the ordinary dispatch path
    // (which materializes the client-context marker into the read-only
    // ClientContext userdata). Reserved kinds have no producer yet and are
    // dropped with a warning.
    void dispatch_client_control(class LuaServiceManager* manager,
                                 const std::string& id,
                                 const ClientControlMessage& msg);

    bool dispatch_message(class LuaServiceManager* manager,
                          const std::string& id, const DispatchMessage& msg);

    void run_fork_task_now(class LuaServiceManager* manager,
                           const ForkedTask& task);

    void run_ready_fork_task(class LuaServiceManager* manager,
                             uint64_t task_id);

    void run_timer_callback_now(class LuaServiceManager* manager,
                                const std::string& service_id,
                                shd::function cb);

    void fire_actor_timer(class LuaServiceManager* manager,
                          const std::string& service_id, uint64_t timer_id);

    // Runs the service's on_exit handler on the actor thread (the
    // ServiceExitRequest handler). Mirrors the inline copy in exit(): errors
    // are swallowed, a nested shield.exit inside on_exit is ignored.
    void run_exit_handler(const std::string& service_id,
                          const std::string& reason);

    // Runs the service's on_shutdown handler on the actor thread (the
    // ServiceDrainRequest handler). Unlike on_exit this is coroutine-aware:
    // the hook may shield.call / shield.sleep within its budget, and the
    // completion routes back to the draining thread through the ordinary
    // call protocol. A missing hook is an immediate no-op success so the
    // drain phase adds no latency for services that do not implement it
    // (a non-function entry counts as absent — the field carries no
    // semantics and shutdown must not fail over it).
    void run_drain_handler(class LuaServiceManager* manager,
                           const std::string& service_id,
                           const ServiceDrainRequest& req);

    // Waits out one service's on_shutdown drain on the calling (shutdown)
    // thread: registers a PendingSyncCall waiter, asks the service actor to
    // run the hook, and blocks until completion or the shared drain
    // deadline. A timeout abandons the wait — the hook keeps running, and
    // its completion later drops safely in complete_call (which no longer
    // finds the session); the stuck actor itself is torn down by
    // shutdown_all right after, never by this path. No expiry driver is
    // armed on purpose: this bounded wait IS the timeout authority, and
    // classifying the outcome locally (ok / error / timeout) beats sniffing
    // the driver's error string.
    void drain_one(const std::string& id, const std::string& reason,
                   std::chrono::steady_clock::time_point deadline);

    static int64_t now_ms();

    Impl(LuaRuntime& rt, caf::actor_system& sys);

    void stop_and_wait_for_actors(const std::vector<caf::actor>& actors);

    // Blocks until every listed actor has terminated. Unlike
    // stop_and_wait_for_actors this never sends an exit signal: used when an
    // actor terminates itself (the ServiceExitRequest handler quits after
    // running on_exit) and an exit signal racing that request would drop it.
    void wait_for_actors(const std::vector<caf::actor>& actors);

    // wait_for_actors with a deadline (nullopt = unbounded, same as
    // wait_for_actors). CAF's wait_for has no timeout overload, so this
    // drives the wait off down_msg monitors and a receive deadline instead.
    // Returns false when the deadline passed with actors still alive.
    bool wait_for_actors_until(
        const std::vector<caf::actor>& actors,
        std::optional<std::chrono::steady_clock::time_point> deadline);

    // Teardown for a service whose on_exit outlived the shutdown budget.
    // The actor thread may still be inside the stuck on_exit, so nothing
    // that could touch its lua_State runs here: fork/timer/RPC/http state
    // stays put (their sol handles are kept valid by the VM parked in
    // hung_vms) and the actor handle is parked instead of joined. The
    // registry drops the service so callers observe it as gone.
    void hung_service_teardown(const std::string& id);

    // Parks a driver handle instead of destroying it on this thread. See the
    // graveyard comment on the member declaration: the release must not happen
    // while any CAF worker may still be inside the driver's message loop.
    void retire_actor(caf::actor&& handle);

    void retire_actors(std::vector<caf::actor>&& handles);

    bool collect_actor_timer_for_cancel(
        uint64_t id, std::vector<caf::actor>* actors_to_stop);

    static std::string resolve_script_path(
        const shield::config::RuntimeActorConfig& actor);

    std::string resolve_module(std::string_view module) const;

    std::string current_service_id() const;

    std::string current_sender_id() const;

    // Server-only helper; the exclusion region lives with the definition.
    std::shared_ptr<LuaVM> current_service_vm() const;

    std::string current_trace_id() const;

    int64_t current_deadline_ms() const;

    static bool valid_name(std::string_view name);

    class DispatchScope {
    public:
        DispatchScope(Impl& impl, std::string service_id, std::string sender_id,
                      bool in_exit, std::string trace_id = "",
                      int64_t deadline_ms = 0,
                      std::shared_ptr<LuaVM> scope_vm = nullptr);

        ~DispatchScope();

        DispatchScope(const DispatchScope&) = delete;
        DispatchScope& operator=(const DispatchScope&) = delete;

    private:
        Impl& impl_;
        std::string service_id_;
    };
};

}  // namespace shield::lua
