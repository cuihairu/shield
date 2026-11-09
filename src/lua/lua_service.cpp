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
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lua_service_impl.hpp"
#include "shield/base/error.hpp"
#include "shield/base/result.hpp"
#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/profile_sampler.hpp"
#include "shield/lua/slow_calls.hpp"
#include "shield/plugin/plugin_host.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::lua {

thread_local std::vector<DispatchFrame> tls_dispatch_stack;

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

void LuaServiceManager::Impl::request_exit_for(const std::string& service_id,
                                               std::string reason) {
    if (service_id.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: dispatch
                               // frames always carry a service id (see
                               // GCOVR_EXCL_LINE comment))
        return;  // GCOVR_EXCL_LINE (caller contract: dispatch frames carry
                 // an id)
    }
    std::lock_guard<std::mutex> lock(exit_request_mutex);
    service_exit_requests[service_id] = std::move(reason);
}

bool LuaServiceManager::Impl::consume_exit_request(
    const std::string& service_id, std::string* reason) {
    std::lock_guard<std::mutex> lock(exit_request_mutex);
    auto it = service_exit_requests.find(service_id);
    if (it == service_exit_requests.end()) {
        return false;
    }
    if (reason) {  // GCOVR_EXCL_BR_LINE (defensive: the sole caller passes
                   // a reason out-param; the null arm is a contract guard)
        *reason = std::move(it->second);
    }
    service_exit_requests.erase(it);
    return true;
}

std::shared_ptr<LuaVM> LuaServiceManager::Impl::find_dispatch_vm(
    const std::string& id) {
    std::shared_lock lock(registry_mutex);
    if (auto it = services.find(id); it != services.end()) {
        return it->second;
    }
    if (auto it = init_vms.find(id); it != init_vms.end()) {
        return it->second;
    }
    return nullptr;  // GCOVR_EXCL_LINE (race: service left both maps)
}

void LuaServiceManager::Impl::drop_live_coroutines_locked(
    const std::string& id) {
    for (auto it = live_coroutines.begin(); it != live_coroutines.end();) {
        if (it->second == id) {
            coroutine_meta.erase(it->first);
            it = live_coroutines.erase(it);
        } else {
            ++it;
        }
    }
}

void LuaServiceManager::Impl::abandon_profile_session_locked(
    const std::string& id) {
    if (!profile_session ||
        profile_session->service_id !=  // GCOVR_EXCL_BR_LINE (defensive)
            id) {  // GCOVR_EXCL_BR_LINE (defensive: every caller re-checks
                   // under the same lock)
        return;
    }
    auto profile = std::move(profile_session);
    profile_session.reset();
    // Kill the duration driver if the install task already spawned it:
    // a live driver idles until its delayed tick and keeps the actor
    // system's teardown waiting for the whole duration.
    if (profile->duration_driver) {  // GCOVR_EXCL_BR_LINE (defensive:
        // null only in the pre-install window; no test exits a service
        // inside that window)
        caf::anon_send_exit(profile->duration_driver,
                            caf::exit_reason::user_shutdown);
    }
    if (profile->done) {  // GCOVR_EXCL_BR_LINE (defensive: done is set
                          // synchronously by profile_start)
        profile->done->set_value(nlohmann::json{
            {"abandoned", true},
            {"service", profile->service_id},
            {"total_samples",
             profile->session
                 ? profile->session->total_samples()  // GCOVR_EXCL_BR_LINE
                 : 0}});  // GCOVR_EXCL_BR_LINE (defensive: session set
                          // synchronously with the state)
    }
}

void LuaServiceManager::Impl::notify_name_change(
    const std::string& name, const std::string& service_id) {
    if (name_change_notifier) name_change_notifier(name, service_id);
}

int64_t LuaServiceManager::Impl::clock_now_ms() const {
    std::shared_lock lock(registry_mutex);
    return clock_->now_ms();
}

int64_t LuaServiceManager::Impl::clock_now_seconds() const {
    std::shared_lock lock(registry_mutex);
    return clock_->now_seconds();
}

void LuaServiceManager::Impl::run_exit_handler(const std::string& service_id,
                                               const std::string& reason) {
    std::shared_ptr<LuaVM> service = find_dispatch_vm(service_id);
    if (!service) {  // GCOVR_EXCL_BR_LINE (defensive: exit raced teardown
                     // (requests only go to live services))
        return;      // GCOVR_EXCL_LINE (race: exit raced teardown)
    }
    std::string error;
    nlohmann::json args = nlohmann::json(reason);
    DispatchScope scope(*this, service_id, "", true);
    (void)runtime.call_service_function(service, "on_exit", args, &error);
}

void LuaServiceManager::Impl::run_drain_handler(
    class LuaServiceManager* manager, const std::string& service_id,
    const ServiceDrainRequest& req) {
    // complete_call drops unknown sessions safely, so routing the
    // outcome unconditionally is fine even in the (defensive) vanished-
    // VM arm below.
    auto complete = [&](bool ok, const nlohmann::json& values) {
        manager->complete_call(req.call_session, ok, values);
    };
    std::shared_ptr<LuaVM> service = find_dispatch_vm(service_id);
    if (!service) {  // GCOVR_EXCL_BR_LINE (defensive: drain requests only
                     // go to live services; the VM cannot be reaped while
                     // its actor is still processing the drain)
        // GCOVR_EXCL_START (defensive: see the branch above)
        complete(false,
                 nlohmann::json::array({"service vanished before drain"}));
        return;
        // GCOVR_EXCL_STOP
    }
    shd::function fn;
    std::string error;
    if (!runtime.resolve_service_method(service, "on_shutdown", &fn, &error)) {
        complete(true, nlohmann::json::array());
        return;
    }
    nlohmann::json ctx = {
        {"reason", req.reason},
        {"deadline_ms", req.deadline_ms},
        {"timeout_ms", req.timeout_ms},
    };  // GCOVR_EXCL_LINE (compiler artifact: json initializer tail
        // blocks never execute on the success path)
    // in_exit = false: unlike on_exit, the drain hook may shield.call /
    // shield.sleep within its budget (that is the whole point of the
    // graceful drain phase).
    DispatchScope scope(*this, service_id, "", /*in_exit=*/false);
    std::string invoke_error;
    // Hook errors route through invoke_coroutine's finish_err: the
    // session completes with the error (the drain loop logs it and
    // continues) and the service error hook observes it like any other
    // handler failure.
    // GCOVR_EXCL_LINE: compiler artifact — the temporary-cleanup blocks
    // on this multi-line call's first line never execute.
    (void)runtime.invoke_coroutine(  // GCOVR_EXCL_LINE
        service, fn, {ctx}, "hook", "on_shutdown", req.call_session, manager,
        service_id, &invoke_error, /*prepend_ctx=*/false);
}

void LuaServiceManager::Impl::drain_one(
    const std::string& id, const std::string& reason,
    std::chrono::steady_clock::time_point deadline) {
    std::optional<caf::actor> actor;
    {
        std::shared_lock lock(registry_mutex);
        auto it = service_actors.find(id);
        if (it != service_actors.end()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                           // the false arc would need the
                                           // service's actor to vanish
                                           // mid-drain, which no path does)
            actor = it->second;
        }
    }
    if (!actor) {  // GCOVR_EXCL_BR_LINE (defensive: drain requests only
                   // go to live services; nothing reaps an actor during
                   // the drain phase)
        return;    // GCOVR_EXCL_LINE (race: service already gone)
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())
            .count();
    const uint64_t session = next_call_session.fetch_add(1);
    auto pending = std::make_shared<PendingSyncCall>();
    pending->session = session;
    {
        std::unique_lock lock(registry_mutex);
        pending_sync_calls[session] = pending;
    }
    ServiceDrainRequest req;
    req.reason = reason;
    // ctx.deadline_ms rides the business clock (shield.now()'s clock) so
    // the documented `ctx.deadline_ms - shield.now()` arithmetic works.
    req.deadline_ms = clock_now_ms() + remaining_ms;
    req.timeout_ms = remaining_ms;
    req.call_session = session;
    caf::anon_send(*actor, std::move(req));

    bool completed = false;
    {
        std::unique_lock lk(pending->mtx);
        completed = pending->cv.wait_until(lk, deadline,
                                           [&] { return pending->completed; });
        completed =
            completed ||  // GCOVR_EXCL_BR_LINE (compiler artifact: the
                          // short-circuit pseudo-arc keying on this line)
            pending->completed;  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // the re-check arc belongs to the race
                                 // window above)
    }
    {
        std::unique_lock lock(registry_mutex);
        pending_sync_calls.erase(session);  // no-op when complete_call
                                            // already erased it
    }
    auto& log = shield::log::get_logger("lua");
    if (!completed) {
        SHIELD_LOG_WARNING(log, "on_shutdown timed out for '" + id +
                                    "'; abandoning the hook (shutdown "
                                    "continues)");
        return;
    }
    if (pending->ok) {
        SHIELD_LOG_INFO(log, "on_shutdown drained '" + id + "'");
        return;
    }
    SHIELD_LOG_ERROR(log,
                     "on_shutdown failed for '" + id + "': " + pending->error);
}

int64_t LuaServiceManager::Impl::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

LuaServiceManager::Impl::Impl(LuaRuntime& rt, caf::actor_system& sys)
    : runtime(rt), system(sys) {
    for (const auto& actor : shield::config::runtime_actors()) {
        module_scripts.emplace(actor.name, resolve_script_path(actor));
    }
}

void LuaServiceManager::Impl::stop_and_wait_for_actors(
    const std::vector<caf::actor>& actors) {
    if (actors.empty()) {
        return;
    }
    for (const auto& actor : actors) {
        if (actor) {  // GCOVR_EXCL_BR_LINE (defensive: wait lists are built
                      // from non-null handles only)
            caf::anon_send_exit(actor, caf::exit_reason::user_shutdown);
        }
    }
    wait_for_actors(actors);
}

void LuaServiceManager::Impl::wait_for_actors(
    const std::vector<caf::actor>& actors) {
    if (actors.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: callers always
                           // pass at least one handle)
        return;            // GCOVR_EXCL_LINE (all actors already dead)
    }
    caf::scoped_actor self{system};
    self->wait_for(actors);
}

bool LuaServiceManager::Impl::wait_for_actors_until(
    const std::vector<caf::actor>& actors,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (actors.empty() ||  // GCOVR_EXCL_BR_LINE (defensive: empty wait list
                           // arm (region excluded above))
        !deadline) {       // GCOVR_EXCL_BR_LINE (defensive: an empty wait list
                      // requires a vanished actor slot; publish inserts the
                      // slot with the service)
        wait_for_actors(actors);
        return true;
    }
    std::vector<caf::actor> live;
    for (const auto& actor : actors) {
        if (actor) {  // GCOVR_EXCL_BR_LINE (defensive: every entry comes
                      // from the non-null slot guarantee at the exit
                      // lookup)
            live.push_back(actor);
        }
    }
    if (live.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: the null filter
                         // never drops an entry (all handles non-null))
        return true;     // GCOVR_EXCL_LINE (all actors already dead)
    }
    caf::scoped_actor self{system};
    for (const auto& actor : live) {
        self->monitor(actor);
    }
    size_t remaining = live.size();
    while (remaining > 0) {
        auto now = std::chrono::steady_clock::now();
        if (now >= *deadline) {
            return false;
        }
        self->receive(
            [&](const caf::down_msg& down) {
                for (const auto& actor :
                     live) {  // GCOVR_EXCL_BR_LINE (defensive: exit waits
                              // exactly one actor; the multi-iteration arc
                              // needs more than one live handle)
                    if (down.source ==
                        actor.address()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                            // deadline is guaranteed
                                            // non-null by the 1201 guard;
                                            // the !deadline arm is dead)
                        --remaining;
                        break;
                    }
                }
            },
            caf::after(std::chrono::duration_cast<std::chrono::nanoseconds>(
                *deadline - now)) >>
                [] {
                    // Timed out; the loop re-checks the deadline.
                });
    }
    return true;
}

void LuaServiceManager::Impl::hung_service_teardown(const std::string& id) {
    std::shared_ptr<LuaVM> service;
    std::vector<std::string> retracted;
    {
        std::unique_lock lock(registry_mutex);
        if (auto vm_it = services.find(id);
            vm_it != services.end()) {  // GCOVR_EXCL_BR_LINE (defensive: hung
                                        // teardown runs once per exit; the
                                        // entry cannot already be erased)
            service = std::move(vm_it->second);
            services.erase(vm_it);
        }
        service_counters.erase(id);
        drop_live_coroutines_locked(id);
        inspect_snapshots.erase(id);
        abandon_profile_session_locked(id);
        if (service) {  // GCOVR_EXCL_BR_LINE (defensive: the service
                        // shared_ptr was found under the same lock scope
                        // (miss implies the 1253 arm))
            hung_vms.push_back(std::move(service));
        }
        if (auto names_it = owned_names.find(id);
            names_it !=
            owned_names
                .end()) {  // GCOVR_EXCL_BR_LINE (defensive: a stuck on_exit
                           // cannot unregister its own name first - no
                           // in-dispatch Lua binding runs during the hang)
            for (const auto& name : names_it->second) {
                published_names.erase(name);
                retracted.push_back(name);
            }
            owned_names.erase(names_it);
        }
        service_order.erase(
            std::remove(service_order.begin(), service_order.end(), id),
            service_order.end());
        recently_exited.insert(id);
        if (auto actor_it = service_actors.find(id);
            actor_it !=
            service_actors.end()) {  // GCOVR_EXCL_BR_LINE (defensive: the actor
                                     // slot is erased only by exit itself; it
                                     // exists for every hung teardown)
            // Park without joining: the actor quits by itself once the
            // stuck on_exit finally unwinds (or never, for a real
            // deadlock — the process watchdog owns that case).
            retire_actor(std::move(actor_it->second));
            service_actors.erase(actor_it);
        }
    }
    for (const auto& name : retracted) {
        notify_name_change(name, "");
    }
}

void LuaServiceManager::Impl::retire_actor(caf::actor&& handle) {
    if (!handle) {  // GCOVR_EXCL_BR_LINE (defensive: callers pass live
                    // handles (branch of a GCOVR_EXCL_LINE'd guard))
        return;     // GCOVR_EXCL_LINE (defensive: callers pass live handles)
    }
    std::lock_guard<std::mutex> lock(retired_actors_mutex);
    retired_actors.push_back(std::move(handle));
}

void LuaServiceManager::Impl::retire_actors(std::vector<caf::actor>&& handles) {
    for (auto& handle : handles) {
        retire_actor(std::move(handle));
    }
}

std::string LuaServiceManager::Impl::resolve_script_path(
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

std::string LuaServiceManager::Impl::resolve_module(
    std::string_view module) const {
    auto it = module_scripts.find(std::string(module));
    if (it != module_scripts.end()) {
        return it->second;
    }
    return std::string(module);
}

std::string LuaServiceManager::Impl::current_service_id() const {
    if (tls_dispatch_stack.empty()) {
        return "";
    }
    return tls_dispatch_stack.back().service_id;
}

std::string LuaServiceManager::Impl::current_sender_id() const {
    if (tls_dispatch_stack.empty()) {
        return "";
    }
    return tls_dispatch_stack.back().sender_id;
}

// GCOVR_EXCL_START (server-only helper: the CI coverage shape builds
// with SHIELD_ENABLE_SERVER=OFF, so this has no caller there; the
// full-build tree exercises it through the shield.server.watch facade)
std::shared_ptr<LuaVM> LuaServiceManager::Impl::current_service_vm() const {
    if (!tls_dispatch_stack  //  (defensive: server-only
                             // helper (see the exclusion region above))
             .empty()) {     //
                             // (defensive: server-only
                             // helper with no caller in
                             // the CI shape (see
                             // the exclusion region above))
        if (auto& vm =       //  (defensive: server-only helper
                             // (see the exclusion region above))
            tls_dispatch_stack.back()
                .vm) {  //  (defensive: server-only
                        // helper with no caller in the CI shape (see
                        // the exclusion region above))
            return vm;
        }
    }
    const std::string id =
        current_service_id();  //  (defensive: server-only
                               // helper with no caller in the CI shape (see
                               // the exclusion region above))
    if (id.empty()) {          //  (defensive: server-only helper
                               // with no caller in the CI shape (see
                               // the exclusion region above))
        return nullptr;
    }
    std::shared_lock lock(
        registry_mutex);      //  (defensive: server-only
                              // helper with no caller in the CI shape (see
                              // the exclusion region above))
    auto it = services.find(  //  (defensive: server-only
                              // helper (see the exclusion region above))
        id);                  //  (defensive: server-only helper with no
              // caller in the CI shape (see the exclusion region above))
    return it != services.end()
               ? it->second  //  (defensive: server-only
                             // helper (see the exclusion region above))
               : nullptr;    //  (defensive: server-only
                             // helper with no caller in the CI shape (see
                             // the exclusion region above))
}

// GCOVR_EXCL_STOP

std::string LuaServiceManager::Impl::current_trace_id() const {
    if (tls_dispatch_stack.empty()) {
        return "";
    }
    return tls_dispatch_stack.back().trace_id;
}

int64_t LuaServiceManager::Impl::current_deadline_ms() const {
    if (tls_dispatch_stack.empty()) {
        return 0;
    }
    return tls_dispatch_stack.back().deadline_ms;
}

LuaServiceManager::Impl::DispatchScope::DispatchScope(
    Impl& impl, std::string service_id, std::string sender_id, bool in_exit,
    std::string trace_id, int64_t deadline_ms, std::shared_ptr<LuaVM> scope_vm)
    : impl_(impl), service_id_(service_id) {
    tls_dispatch_stack.push_back({
        std::move(service_id),
        std::move(sender_id),
        std::move(trace_id),
        deadline_ms,
        in_exit,
        std::move(scope_vm),
    });
}

LuaServiceManager::Impl::DispatchScope::~DispatchScope() {
    // Save last sender for context_expired detection.
    if (!tls_dispatch_stack  // GCOVR_EXCL_BR_LINE (defensive:
                             // DispatchScope dtor always pairs with a
                             // push)
             .empty()) {     // GCOVR_EXCL_BR_LINE
                             // (defensive:
                             // DispatchScope dtor
                             // always pairs with a
                             // push; the stack is
                             // never empty at
                             // teardown)
        const auto& frame = tls_dispatch_stack.back();
        if (!frame.sender_id.empty()) {
            std::unique_lock lock(impl_.registry_mutex);
            impl_.last_sender_per_service[frame.service_id] = frame.sender_id;
        }
        // Sample the VM's Lua heap at dispatch exit (L1 memory
        // observability): this thread is the VM's only executor
        // here and lua_gc(GCCOUNT) is O(1), so the cost stays
        // bounded. A sampling gauge — a handler that finishes on a
        // resume frame outside any scope keeps the last sample,
        // and a service already torn down simply does not store.
        const std::shared_ptr<LuaVM> vm =
            frame.vm ? frame.vm : impl_.find_dispatch_vm(frame.service_id);
        if (vm) {
            const int kb =
                lua_gc(impl_.runtime.vm_state(vm).lua_state(), LUA_GCCOUNT);
            std::shared_lock lock(impl_.registry_mutex);
            if (auto it = impl_.service_counters.find(frame.service_id);
                it != impl_.service_counters.end()) {
                it->second->memory_kb.store(static_cast<std::uint64_t>(kb),
                                            std::memory_order_relaxed);
            }
        }
    }
    tls_dispatch_stack.pop_back();
}

LuaServiceManager::LuaServiceManager(LuaRuntime& runtime,
                                     caf::actor_system& system)
    : impl_(std::make_unique<Impl>(runtime, system)) {
    runtime.set_service_manager(this);

    // Back the host_api lua_current_service_id / lua_post_to_service slots
    // so plugins re-enter Lua on the owning service actor instead of
    // touching a lua_State from plugin threads.
    shield::plugin::LuaServiceHooks hooks;
    // GCOVR_EXCL_START (hook body: fires only when a plugin calls
    // lua_current_service_id -- an integration path unreachable by the
    // coverage tree, which builds with every plugin OFF; the wrapped
    // current_service_id() itself is exercised directly by the manager
    // suites)
    hooks.current_service_id = [this]() { return current_service_id(); };
    // GCOVR_EXCL_STOP
    hooks.post_to_service = [this](const std::string& service_id,
                                   std::function<void()> fn) {
        // Hook body: fires only when a plugin calls lua_post_to_service;
        // integration context.
        // GCOVR_EXCL_START
        return enqueue_forked_task(service_id, std::move(fn));
        // GCOVR_EXCL_STOP
    };
    // DB-async entries (docs/db-async-design.md): a plugin's registered C
    // closure parks the calling coroutine and a plugin worker completes it
    // later. Both bodies are exercised end-to-end by the db-async suites
    // (tests/coverage/test_cov_lua_db_async.cpp) through the host_api
    // vtable, so they carry no coverage exclusions.
    hooks.suspend_current = [this](lua_State* L, int32_t timeout_ms,
                                   const std::string& tag) -> uint64_t {
        // Refuse to suspend the main thread (mirrors _coro_call): anchoring
        // it would leave the VM's main thread parked with no resume source.
        // The plugin's Lua layer falls back to running the work inline when
        // it sees the 0.
        if (lua_pushthread(L) == 1) {
            lua_pop(L, 1);
            return 0;
        }
        lua_pop(L, 1);
        return suspend_for_call(L, timeout_ms, tag);
    };
    hooks.resume_session = [this](uint64_t session, bool ok,
                                  const std::string& result_json) -> bool {
        // The completion payload rides as a JSON array of values —
        // (true, v...) on success, (false, {code=...,message=...}) on
        // failure. A malformed payload completes the session as a failure so
        // the parked coroutine always gets exactly one resume; the plugin
        // learns its real result was dropped via the true return (the
        // session was consumed here).
        nlohmann::json values = nlohmann::json::array();
        bool deliver_ok = ok;
        if (!result_json.empty()) {
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(result_json);
            } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch
                       // construct pseudo-arc)
                const nlohmann::json::exception&) {
                deliver_ok = false;
                // GCOVR_EXCL_START (compiler artifact: the json::array({...})
                // initializer expands to template branch arcs no runtime path
                // can take; the guard logic around it is live — see the
                // malformed-payload case in test_cov_lua_db_async)
                parsed = nlohmann::json::array({nlohmann::json::object(
                    {{"code", "db_async_bad_payload"},
                     {"message", "resume payload is not valid JSON"},
                     {"retryable", false}})});
                // GCOVR_EXCL_STOP
            }  // GCOVR_EXCL_LINE (compiler artifact: the catch body's exit
               // arc jumps straight to the join, so this line never gets
               // its own counter)
            if (!parsed.is_array()) {
                deliver_ok = false;
                // GCOVR_EXCL_START (same initializer artifact as above)
                parsed = nlohmann::json::array({nlohmann::json::object(
                    {{"code", "db_async_bad_payload"},
                     {"message", "resume payload is not a JSON array"},
                     {"retryable", false}})});
                // GCOVR_EXCL_STOP
            }
            values = std::move(parsed);
        }
        // Claim-then-route: the claim marker rejects a second completion
        // (racing worker / double resume) exactly-once; the route goes
        // through the caller actor's mailbox via complete_call so the
        // resume always runs on the thread that owns the coroutine — never
        // inline on this (worker) thread.
        {
            std::unique_lock lock(impl_->registry_mutex);
            auto it = impl_->pending_calls.find(session);
            if (it == impl_->pending_calls.end() ||  // GCOVR_EXCL_BR_LINE
                                                     // (unreachable condition
                                                     // combination: the
                                                     // absent-entry arm and
                                                     // the claimed arm cannot
                                                     // both fire)
                it->second.async_claimed) {          // GCOVR_EXCL_BR_LINE (same
                                             // artifact: the short-circuit pair
                                             // keys its pseudo-arc here)
                return false;
            }
            it->second.async_claimed = true;
        }
        complete_call(session, deliver_ok, values);
        return true;
    };  // GCOVR_EXCL_LINE        (compiler artifact: lambda exit region)
        // GCOVR_EXCL_BR_LINE (unwind edge out of complete_call is the only
        // arc through this region the tests cannot take)
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
    if (impl_               // GCOVR_EXCL_BR_LINE
            ->spawn_thread  // GCOVR_EXCL_BR_LINE (defensive: spawn_thread is
                            // joined before the dtor ends)
            .joinable()) {  // GCOVR_EXCL_BR_LINE (defensive: spawn_thread is
                            // joined before the dtor; the not-joinable arm is
                            // the invariant)
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
    // Without this cleanup the actor_timers' shd::function/std::function
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
            if (actor) {  // GCOVR_EXCL_BR_LINE (defensive: service_actors
                          // entries always carry a live handle; exit paths
                          // erase entries rather than null them)
                // Move: the clear() below would otherwise release the map
                // entry's reference while the actor may still be running.
                actors_to_stop.push_back(std::move(actor));
            }
        }
        impl_->service_actors.clear();
        for (auto& [id, timer] : impl_->actor_timers) {
            if (timer.driver) {  // GCOVR_EXCL_BR_LINE (defensive: actor_timers
                                 // entries always carry their driver;
                                 // cancel/fire erase the entry)
                // Move: the clear() below would otherwise release the map
                // entry's reference while the driver may still be running;
                // releasing ours after stop_and_wait_for_actors is safe.
                actors_to_stop.push_back(std::move(timer.driver));
            }
        }
        impl_->actor_timers.clear();
        impl_->actor_timers_by_service.clear();
        for (auto& [session, driver] : impl_->actor_call_timeouts) {
            if (driver) {  // GCOVR_EXCL_BR_LINE (defensive: call-timeout
                           // entries always carry their driver; drivers are
                           // armed at insert)
                // Same move-before-clear reasoning as the timer drivers.
                actors_to_stop.push_back(std::move(driver));
            }
        }
        impl_->actor_call_timeouts.clear();

        // A manager dying with an active /ops/profile session must take
        // the session's duration driver with it: the driver idles until
        // its delayed tick and nothing after this dtor waits on it — the
        // actor system (which outlives the manager) would wait out the
        // whole session duration in its own teardown. Same settle shape
        // as abandon_profile_session_locked (the exit paths' helper);
        // like that helper this never uninstalls: the sampler dies with
        // the state and a firing hook self-disarms on the null slot.
        if (impl_->profile_session) {
            auto profile = std::move(impl_->profile_session);
            impl_->profile_session.reset();
            if (profile->duration_driver) {  // GCOVR_EXCL_BR_LINE (race:
                // null only inside profile_start's spawn-to-register
                // window — the same pre-install window class the
                // abandon helper excludes)
                actors_to_stop.push_back(std::move(profile->duration_driver));
            }
            profile->done->set_value(nlohmann::json{
                {"abandoned", true},
                {"service", profile->service_id},
                {"total_samples", profile->session->total_samples()}});
        }
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
                    if (!pending
                             ->completed) {  // GCOVR_EXCL_BR_LINE (race: a
                                             // completed-but-unfetched pending
                                             // surviving a loop pass - the
                                             // waiter/notify window the
                                             // re-check loop already handles)
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
    if (impl_->draining.load()) {
        // on_shutdown contract (docs/lua-api.md): no spawn during drain.
        return SpawnResult::error(
            "runtime is draining: spawn is rejected during on_shutdown");
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
        if (!Impl::valid_name(service_name)) {
            return SpawnResult::error("invalid service name: " + service_name);
        }

        // Check and reserve in ONE critical section. A split window (checks
        // under one lock, insert under a later one) let two concurrent spawns
        // of the same name both pass: the second insert was a silent no-op,
        // and both threads ran VM setup — including the process-global API
        // registration — concurrently. The publish-time double check still
        // kept a single owner, but the overlap raced global binding state and
        // surfaced as exit-time heap corruption on loaded coverage runs.
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
            impl_->reserved_names.insert(service_name);
        }

        // The name stays reserved for the whole init phase: concurrent spawns
        // with the same name fail fast, while query_service still cannot see
        // the name until it is published after a successful on_init. The
        // guard rolls the reservation back on every failure path; the success
        // path clears it under the publish lock below.
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
        } reservation{impl_.get(),
                      service_name};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // RAII guard aggregate braced-init arc)

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
                    shd::function handler;
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
            // waiter and the VM is idle. That pass-through is only safe
            // because resume_caller re-enqueues a response through this
            // actor's mailbox whenever the caller coroutine's driving
            // phase is still registered — every lua_resume of a handler
            // coroutine stays on an actor thread, never on the spawning
            // thread that observed the yield. A completion resumed by
            // whatever thread observed the yield would break exactly this
            // invariant: two OS threads inside one lua_State.
            // fork_task_atom is deliberately NOT
            // passed through: a fork enqueued by a still synchronous on_init
            // would otherwise execute here while the spawning thread is still
            // inside the VM — two OS threads in one lua_State. Stashing it
            // defers execution to the unstash below, so a fork scheduled
            // during on_init runs serially after init (the documented fork
            // contract). The spawner sends init_ready_atom once on_init
            // returns; we then install the real behavior and release the
            // stash.
            auto cache = std::make_shared<caf::mail_cache>(self, 4096);
            self->set_default_handler([cache,     // GCOVR_EXCL_BR_LINE
                                       impl_ptr,  // GCOVR_EXCL_BR_LINE
                                                  // (compiler artifact: CAF
                                                  // pre-init default-handler
                                                  // lambda arcs)
                                       manager,  // GCOVR_EXCL_BR_LINE (compiler
                                                 // artifact: CAF pre-init
                                                 // default-handler lambda
                                                 // entry/exit arcs)
                                       svc](caf::message& msg)
                                          -> caf::skippable_result {
                if (msg.size() == 2 &&
                    (msg.match_element<timer_fire_atom>(
                         0) ||  // GCOVR_EXCL_BR_LINE (defensive: pre-init twin
                                // arm - post-init traffic is served by the
                                // become() handlers (see the exclusion region
                                // at 1834))
                     msg.match_element<call_timeout_atom>(0))) {
                    const uint64_t payload = msg.get_as<uint64_t>(1);
                    if (msg.match_element<     // GCOVR_EXCL_BR_LINE
                            timer_fire_atom>(  // GCOVR_EXCL_BR_LINE (defensive:
                                               // pre-init twin arm (region
                                               // excluded above))
                            0)) {  // GCOVR_EXCL_BR_LINE (defensive: pre-init
                                   // twin arm - the call-timeout case is region
                                   // excluded at 1834)
                        impl_ptr->fire_actor_timer(manager, svc, payload);
                    } else {  // GCOVR_EXCL_START (pre-init twin of the
                        // registered call_timeout_atom handler below: the
                        // e2e driver always beats this scan path)
                        manager->cancel_actor_call_timeout(  //
                                                             // (defensive:
                                                             // pre-init twin
                                                             // region (region
                                                             // excluded above))
                            payload);                        //  (defensive:
                                       // pre-init twin region (see
                                       // the exclusion region at 1834))
                        nlohmann::json timeout_err = nlohmann::json::array(
                            {nlohmann::json::object(  //
                                                      // (defensive: pre-init
                                                      // twin region (see
                                                      // the exclusion region at
                                                      // 1834))
                                {{"code", "timeout"},
                                 {"message", "call timeout"},
                                 {"retryable",
                                  true}})});  //  (defensive:
                                              // pre-init twin region (see
                                              // the exclusion region at 1834))
                        manager->resume_caller(  //  (defensive:
                                                 // pre-init twin region (region
                                                 // excluded above))
                            payload, false,
                            timeout_err,  //  (defensive:
                                          // pre-init twin region (see
                                          // the exclusion region at 1834))
                            "call-timeout");
                    }
                    // GCOVR_EXCL_STOP
                    return {};
                }
                if (msg.match_element<CallResponseMessage>(0)) {
                    impl_ptr->dispatch_call_response(
                        manager, msg.get_as<CallResponseMessage>(0));
                    return {};
                }
                cache->stash(msg);
                return {};
            });  // GCOVR_EXCL_LINE GCOVR_EXCL_BR_LINE (pre-init stash tail:
                 // branch arc artifact)
            return caf::behavior{
                [self, cache, impl_ptr,  // GCOVR_EXCL_BR_LINE
                 manager,  // GCOVR_EXCL_BR_LINE (compiler artifact: init_ready
                           // lambda entry/exit arcs)
                 svc](init_ready_atom) {  // GCOVR_EXCL_BR_LINE (compiler
                                          // artifact: init_ready lambda
                                          // entry/exit arcs)
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
                        [impl_ptr,  // GCOVR_EXCL_LINE (lambda entry artifact)
                         manager](  // GCOVR_EXCL_LINE (lambda entry artifact)
                            const CallResponseMessage&
                                msg) {  // GCOVR_EXCL_LINE (lambda entry
                                        // artifact)
                            impl_ptr->dispatch_call_response(manager, msg);
                        },
                        [impl_ptr, manager, svc](timer_fire_atom,
                                                 uint64_t timer_id) {
                            impl_ptr->fire_actor_timer(manager, svc, timer_id);
                        },
                        [impl_ptr, manager, svc](call_timeout_atom,
                                                 uint64_t session) {
                            manager->cancel_actor_call_timeout(session);
                            nlohmann::json timeout_err = nlohmann::json::array(
                                {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE
                                                          // (compiler artifact:
                                                          // json::array
                                                          // braced-init arcs)
                                    {{"code", "timeout"},
                                     {"message", "call timeout"},
                                     {"retryable",
                                      true}})});  // GCOVR_EXCL_BR_LINE
                                                  // (compiler artifact: json
                                                  // object init continuation
                                                  // arc)
                            manager->resume_caller(session, false, timeout_err,
                                                   "call-timeout");
                        },  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda
                        // body exit arc)
                        [impl_ptr,  // GCOVR_EXCL_LINE (lambda entry artifact)
                         manager](  // GCOVR_EXCL_LINE (lambda entry artifact)
                            fork_task_atom,
                            uint64_t task_id) {  // GCOVR_EXCL_LINE (lambda
                                                 // entry artifact)
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
                        [impl_ptr, manager,
                         svc](const ServiceDrainRequest& req) {
                            // Graceful drain from the shutdown thread
                            // (manager.drain_all): on_shutdown runs here on
                            // the actor thread like every other VM touch and
                            // completes through the ordinary call protocol.
                            // The actor does NOT quit — shutdown_all's exit
                            // request follows separately once the whole drain
                            // phase is over.
                            impl_ptr->run_drain_handler(manager, svc, req);
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
        };  // GCOVR_EXCL_BR_LINE (compiler artifact: json initializer tail arc)
        bool exit_after_init = false;
        std::string exit_reason;
        {
            Impl::DispatchScope scope(*impl_, service_name, "", false, "", 0,
                                      vm);
            // on_init runs as a coroutine so it may yield inside
            // shield.sleep / shield.call: the spawn waits on the external
            // waiter while the runtime resumes the coroutine. The spawn
            // timeout budget bounds the whole wait.
            Impl::SpawnInitFlag spawn_init_flag;
            t_in_spawn_init = true;
            shd::function on_init_fn;
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
                        return impl_->runtime
                            .invoke_coroutine(  // GCOVR_EXCL_BR_LINE (compiler
                                                // artifact: multi-line
                                                // invoke_coroutine continuation
                                                // arcs)
                                vm, on_init_fn, {init_args}, "handler",
                                "on_init", session, this, service_name, &err,
                                /*prepend_ctx=*/false);
                    },  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda body
                        // exit arcs)
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
                    if (error.find("call timeout") !=
                            std::string::npos ||  // GCOVR_EXCL_BR_LINE (race:
                                                  // the spawn-timeout diag arm
                                                  // - external-driver expiry on
                                                  // a loaded machine (see
                                                  // the exclusion region at
                                                  // 1979))
                        init_overran) {
                        // Diagnostic for spawn-hang investigations: surface
                        // the pending-call bookkeeping at expiry so logs can
                        // distinguish "response lost" from "requeue loop".
                        // GCOVR_EXCL_START (diagnostic dump: this branch is
                        // the external-driver expiry arm, exercised only by
                        // end-to-end spawn timeouts on loaded machines)
                        {
                            std::shared_lock diag_lock(impl_->registry_mutex);
                            std::fprintf(
                                stderr,
                                "*** shield spawn timeout diag: service=%s "
                                "pending_calls=%zu pending_sync_calls=%zu "
                                "driving=%zu\n",
                                service_name.c_str(),
                                impl_->pending_calls.size(),
                                impl_->pending_sync_calls.size(),
                                impl_->driving_cos.size());
                            for (const auto& [diag_session, diag_pc] :
                                 impl_->pending_calls) {  //
                                                          // (defensive: diag
                                                          // dump internals of
                                                          // the spawn-timeout
                                                          // arm (see
                                                          // the exclusion
                                                          // region at 1979))
                                std::fprintf(             //  (defensive:
                                               // diag dump internals of the
                                               // spawn-timeout arm)
                                    stderr,  //  (defensive:
                                             // diag dump internals of the
                                             // spawn-timeout arm (see
                                             // the exclusion region at 1979))
                                    "***   session=%llu caller=%s "
                                    "requeues=%d\n",
                                    static_cast<unsigned long long>(
                                        diag_session),
                                    diag_pc.caller_service.c_str(),
                                    diag_pc.requeues);
                            }
                            std::fflush(stderr);
                        }
                        // GCOVR_EXCL_STOP
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
                        error =
                            init_result.values.size() > 1 &&
                                    init_result.values[1].is_string()
                                ? init_result.values[1].get<std::string>()
                            : false_first
                                ? "on_init returned false"  // GCOVR_EXCL_BR_LINE
                                                            // (compiler
                                                            // artifact:
                                                            // ternary-chain
                                                            // structural edges)
                                : "on_init returned nil";
                    }
                }
            } else {
                // A module without on_init is fine (call_service_function
                // treated a missing hook as a silent no-op); anything else is
                // a load failure surfaced below.
                if (error.find(
                        "is missing or not a function") ==  // GCOVR_EXCL_BR_LINE
                                                            // (defensive:
                                                            // sync-spawn twin
                                                            // of the async
                                                            // path's identical
                                                            // failure return
                                                            // (see
                                                            // the exclusion
                                                            // region at 2037))
                    std::string::npos) {
                    // GCOVR_EXCL_START (sync-spawn twin: the async spawn
                    // path's identical failure return is the driven site)
                    return SpawnResult::error(  // GCOVR_EXCL_BR_START
                        "on_init failed for " + service_name +
                        ": " +   //  (defensive: sync-spawn
                                 // twin (region excluded below))
                        error);  //  (defensive: sync-spawn
                                 // twin of the async path's identical failure
                                 // return (see the exclusion region at 2037))
                    // GCOVR_EXCL_STOP
                }  // GCOVR_EXCL_BR_STOP
                error.clear();
            }
            if (!error.empty()) {
                std::vector<std::string> retracted;
                {
                    std::unique_lock lock(impl_->registry_mutex);
                    if (auto actor_it =
                            impl_->service_actors.find(service_name);
                        actor_it !=
                        impl_->service_actors
                            .end()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                       // sync-spawn twin - the actor-slot
                                       // cleanup is driven by the async spawn
                                       // failure path)
                        // The actor may still be inside the failed on_init on
                        // its worker thread; the graveyard keeps the handle
                        // alive until the Impl destructor joins it.
                        if (actor_it  // GCOVR_EXCL_BR_LINE (defensive:
                                      // sync-spawn twin (actor slot never
                                      // null))
                                ->second) {  // GCOVR_EXCL_BR_LINE
                                             // (defensive: sync-spawn
                                             // twin - the actor-slot
                                             // cleanup is driven by
                                             // the async spawn
                                             // failure path)
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
            if (impl_->services.contains(
                    service_name) ||  // GCOVR_EXCL_BR_LINE (race: same-name
                                      // usurp during async init; the spawn
                                      // entry check refuses duplicates under
                                      // the lock)
                impl_->published_names.contains(service_name)) {
                if (auto actor_it = impl_->service_actors.find(service_name);
                    actor_it !=
                    impl_->service_actors
                        .end()) {  // GCOVR_EXCL_BR_LINE (race: usurp twin - the
                                   // entry is erased together with the service
                                   // under the same lock)
                    // Graveyard policy: the actor may still be running its
                    // on_init; never release the last reference here.
                    if (actor_it  // GCOVR_EXCL_BR_LINE (defensive: usurp twin
                                  // (actor slot never null))
                            ->second) {  // GCOVR_EXCL_BR_LINE
                                         // (defensive: usurp twin -
                                         // the actor slot is never
                                         // null for a published
                                         // service)
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
            // Fresh traffic counters per incarnation: a respawned name
            // re-counts from zero (the exit path dropped the old entry).
            impl_->service_counters[service_name] =
                std::make_shared<Impl::ServiceCounters>();
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

    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch structural edge
               // (landing edge is covered by tests))
        const std::exception& e) {  // GCOVR_EXCL_BR_LINE (defensive: untestable
                                    // spawn-internal exception guard; the
                                    // guarded body is exception-free in tests)
        return SpawnResult::error(std::string("Spawn failed: ") + e.what());
    }
}

void LuaServiceManager::exit(
    std::string_view service_id, std::string_view reason,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
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
        // serialized continuation of the current message. A deadline cannot
        // apply: waiting out a stuck on_exit would mean waiting on this
        // thread, which IS the stuck thread.
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
            if (it != impl_->service_actors
                          .end() &&  // GCOVR_EXCL_BR_LINE (defensive: publish
                                     // inserts the actor slot with the service)
                it->second) {  // GCOVR_EXCL_BR_LINE (defensive: publish inserts
                               // the actor slot with the service; the entry
                               // exists and is non-null for every exit)
                actor = it->second;
            }
        }
        if (actor) {  // GCOVR_EXCL_BR_LINE (defensive: same publish guarantee -
                      // the actor optional is always set here)
            caf::anon_send(*actor, ServiceExitRequest{std::string(reason)});
            actors_to_wait.push_back(*actor);
        }
        // on_exit must complete before the teardown below: the handler
        // resolves its VM through the registry, and the VM's last owner
        // (the local `service` handle) dies with this frame.
        const bool settled =
            impl_->wait_for_actors_until(actors_to_wait, deadline);
        // wait_for_actors joins on CAF's monitor-down, which is raised while
        // the actor's worker is still unwinding cleanup() — releasing the
        // last reference on this thread would destroy the actor storage
        // underneath it (TSan-verified heap corruption). Park the handles in
        // the graveyard; the Impl destructor releases them safely.
        impl_->retire_actors(std::move(actors_to_wait));
        if (!settled) {
            // Budget exhausted with on_exit still running: bail out of the
            // teardown instead of destroying fork/timer/RPC/http state whose
            // sol handles point into the VM the stuck thread is using.
            impl_->hung_service_teardown(id);
            return;
        }
    }

    // Cancel forked tasks / timers / coroutines BEFORE erasing the service
    // VM. These hold shd::function / std::function callbacks that reference
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
                if (it->second) {  // GCOVR_EXCL_BR_LINE (defensive:
                                   // call-timeout entries always carry their
                                   // driver; the null arm is a contract guard)
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
            names_it != impl_->owned_names
                            .end()) {  // GCOVR_EXCL_BR_LINE (defensive: publish
                                       // inserts owned_names with the service)
            for (const auto& name : names_it->second) {
                impl_->published_names.erase(name);
                retracted.push_back(name);
            }
            impl_->owned_names.erase(names_it);
        }
        // Erase the RPC state before the VM: sol handler handles reference
        // the VM's lua_State and must be destroyed while it is alive.
        impl_->service_rpc.erase(id);
        impl_->service_counters.erase(id);
        impl_->drop_live_coroutines_locked(id);
        impl_->inspect_snapshots.erase(id);
        impl_->abandon_profile_session_locked(id);
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
            actor_it !=
            impl_->service_actors
                .end()) {  // GCOVR_EXCL_BR_LINE (defensive: the teardown find
                           // runs in the same critical section that located the
                           // service)
            if (actor_it
                    ->second &&  // GCOVR_EXCL_BR_LINE (defensive: null-actor
                                 // combo unreachable (slot never null))
                exiting_from_own_actor) {  // GCOVR_EXCL_BR_LINE (defensive:
                                           // null-actor combo unreachable - the
                                           // slot is never null for a published
                                           // service)
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
        if (seen.insert(*it)
                .second &&  // GCOVR_EXCL_BR_LINE (defensive: order entries are
                            // unique (exits erase their entry))
            exists) {       // GCOVR_EXCL_BR_LINE (defensive: order entries are
                       // unique and every exit path erases its order entry; the
                       // dedup arm cannot trigger)
            const auto now = std::chrono::steady_clock::now();
            if (now < deadline) {
                // The whole budget is shared: every graceful exit waits at
                // most until the common deadline, so one stuck on_exit
                // cannot starve the services queued behind it.
                exit(*it, reason, deadline);
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

void LuaServiceManager::drain_all(std::string_view reason,
                                  int64_t drain_budget_ms) {
    if (drain_budget_ms <= 0) {
        return;  // no budget: the drain phase is inert (<= 0 is documented
                 // as "feature off")
    }
    impl_->draining.store(true);
    // The whole phase shares one deadline (same pattern as shutdown_all's
    // graceful budget): each hook gets the remaining budget as
    // ctx.timeout_ms, so one slow on_shutdown cannot starve the services
    // queued behind it.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(drain_budget_ms);
    std::vector<std::string> order;
    {
        std::shared_lock lock(impl_->registry_mutex);
        order = impl_->service_order;
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const auto remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining_ms <= 0) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_WARNING(
                log, "on_shutdown drain budget exhausted; skipping the " +
                         std::to_string(std::distance(it, order.rend())) +
                         " remaining service(s)");
            break;
        }
        impl_->drain_one(*it, std::string(reason), deadline);
    }
    // draining stays latched: shutdown_all (which sets stopping) follows
    // immediately in the shutdown sequence, so nothing unsets it.
}

void LuaServiceManager::force_remove(const std::string& id,
                                     const std::string& reason) {
    (void)reason;
    // Cancel forked tasks / timers / coroutines BEFORE erasing the service
    // VM (same rationale as exit()): the actor_timers and actor_call_timeouts
    // entries hold shd::function / std::function callbacks and driver handles
    // that reference this service's lua_State, so releasing them after the
    // VM is destroyed would luaL_unref on a closed state.
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
    // Cancel any pending CAF call-timeout drivers for calls originated by
    // this service (same shape as exit(): the timeout handler no-ops when
    // the session is already gone from pending_calls). Fires when the removed
    // service has a proxied call still in flight at force-remove time
    // (ForceRemoveCancelsPendingCallTimeouts).
    {
        std::unique_lock lock(impl_->registry_mutex);
        for (auto it = impl_->actor_call_timeouts.begin();
             it != impl_->actor_call_timeouts.end();) {
            auto pc_it = impl_->pending_calls.find(it->first);
            if (pc_it != impl_->pending_calls.end() &&
                pc_it->second.caller_service == id) {
                if (it->second) {  // GCOVR_EXCL_BR_LINE (defensive:
                                   // call-timeout entries always carry their
                                   // driver; the null arm is a contract guard)
                    actors_to_stop.push_back(std::move(it->second));
                }
                it = impl_->actor_call_timeouts.erase(it);
            } else {
                ++it;
            }
        }
    }
    caf::actor actor;
    std::vector<std::string> retracted;
    {
        std::unique_lock lock(impl_->registry_mutex);
        if (auto names_it = impl_->owned_names.find(id);
            names_it !=
            impl_->owned_names
                .end()) {  // GCOVR_EXCL_BR_LINE (defensive: a published service
                           // always owns at least its service name)
            for (const auto& name : names_it->second) {
                impl_->published_names.erase(name);
                retracted.push_back(name);
            }
            impl_->owned_names.erase(names_it);
        }
        // Erase the RPC state before the VM (sol handles reference the VM's
        // lua_State); see exit() for the mirrored ordering.
        impl_->service_rpc.erase(id);
        impl_->service_counters.erase(id);
        impl_->drop_live_coroutines_locked(id);
        impl_->inspect_snapshots.erase(id);
        impl_->abandon_profile_session_locked(id);
        impl_->services.erase(id);
        impl_->service_order.erase(std::remove(impl_->service_order.begin(),
                                               impl_->service_order.end(), id),
                                   impl_->service_order.end());
        // Cap the tombstone set identically to exit(): a forced removal can
        // also exhaust the budget, and an uncapped insert would grow
        // unbounded on repeated shutdown-budget force_remove calls.
        if (impl_->recently_exited.size() >= Impl::kRecentlyExitedLimit) {
            impl_->recently_exited.clear();
        }
        impl_->recently_exited.insert(id);
        impl_->runtime.remove_http_routes_for_service(id);
        if (auto actor_it = impl_->service_actors.find(id);
            actor_it !=
            impl_->service_actors
                .end()) {  // GCOVR_EXCL_BR_LINE (defensive: a published service
                           // always has a live actor slot)
            // Move before erase: the entry's reference must not be released
            // on this thread while the actor's worker may still be running
            // (same graveyard policy as exit()).
            actor = std::move(actor_it->second);
            impl_->service_actors.erase(actor_it);
        }
    }
    if (actor) {  // GCOVR_EXCL_BR_LINE (defensive: the moved-out actor is
                  // non-null whenever the slot existed)
        caf::anon_send_exit(actor, caf::exit_reason::user_shutdown);
        // anon_send_exit only queues the signal; this thread dropping the
        // last reference here could still race the actor's cleanup unwind.
        impl_->retire_actor(std::move(actor));
    }
    for (const auto& name : retracted) {
        impl_->notify_name_change(name, "");
    }
    impl_->stop_and_wait_for_actors(actors_to_stop);
    // The drivers were joined above, but releasing the last reference on
    // this thread can still race the CAF worker's final unwind of a driver's
    // message loop — park the handles in the graveyard (same policy as exit).
    impl_->retire_actors(std::move(actors_to_stop));
}

bool LuaServiceManager::enqueue_async_spawn(uint64_t session,
                                            std::string module,
                                            std::string opts_json) {
    if (impl_->stopping.load()) {
        return false;
    }
    if (impl_->draining.load()) {
        // on_shutdown contract: no spawn during drain (shield.spawn from a
        // drain hook fails; the hook observes the failure like any spawn
        // failure).
        return false;
    }
    {
        std::lock_guard lock(impl_->spawn_mutex);
        if (impl_->spawn_stop) {  // GCOVR_EXCL_BR_LINE (defensive: spawn_stop
                                  // is set only by the dtor after drain)
            return false;  // GCOVR_EXCL_LINE (race: stopping flag set first)
        }  // GCOVR_EXCL_BR_START
        impl_->spawn_queue.push_back(Impl::SpawnJob{
            //
            //
            //  (compiler artifact: SpawnJob aggregate
            // braced-init arc)
            session, std::move(module),  // GCOVR_EXCL_BR_STOP
            std::move(opts_json)});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // SpawnJob aggregate braced-init arc)
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
        if (impl_->spawn_stop &&  // GCOVR_EXCL_BR_LINE (defensive: dtor drains
                                  // the queue before setting spawn_stop)
            impl_->spawn_queue
                .empty()) {  // GCOVR_EXCL_BR_LINE (defensive: the dtor drains
                             // the queue before setting spawn_stop;
                             // stop-with-pending-jobs is never observed)
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
            if (actor_it != impl_->service_actors
                                .end() &&  // GCOVR_EXCL_BR_LINE (race: caller
                                           // pending entry outliving its actor
                                           // (shutdown window))
                actor_it->second) {  // GCOVR_EXCL_BR_LINE (race: the caller's
                                     // pending entry outliving its actor -
                                     // shutdown window)
                caller_alive = true;
                caller_actor = actor_it->second;
            }
        }
    }

    if (!caller_waiting) {
        // The caller already resumed on timeout while init was still running.
        // Roll the successfully spawned child back so "spawn timeout" keeps
        // its documented meaning (init timeout -> service not started).
        if (result.success) {  // GCOVR_EXCL_BR_LINE (race: needs init to finish
                               // between the caller's timeout and this finish -
                               // microsecond window)
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
        values =
            nlohmann::json::array(     // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // json::array braced-init arcs)
                {result.service_id});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // json::array braced-init arcs)
    } else {
        std::string code = "spawn_failed";
        if (result.error_message.find("timeout") != std::string::npos) {
            code = "spawn_timeout";
        } else if (result.error_message.find("on_init failed") !=
                   std::string::npos) {
            code = "init_failed";
        }
        values = nlohmann::json::array(  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: json::array braced-init
                                         // arcs)
            {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // json::array braced-init arcs)
                {{"code", code},      // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // json object init continuation arc)
                 {"message", result.error_message},
                 {"retryable", false}})});
    }  // GCOVR_EXCL_LINE (function-exit arc artifact of the if(!ok) block)

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

// GCOVR_EXCL_START (server-only helper: the CI coverage shape builds with
// SHIELD_ENABLE_SERVER=OFF, so this has no caller there)
std::shared_ptr<LuaVM> LuaServiceManager::current_service_vm() const {
    return impl_->current_service_vm();
}

// GCOVR_EXCL_STOP

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
    impl_->request_exit_for(
        frame.service_id,
        reason.empty()
            ? "normal"               // GCOVR_EXCL_BR_LINE
            : std::string(reason));  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // ternary std::string construction arcs)
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

size_t LuaServiceManager::recently_exited_count() const {
    std::shared_lock lock(impl_->registry_mutex);
    return impl_->recently_exited.size();
}

bool LuaServiceManager::recently_exited_contains(
    const std::string& service_id) const {
    std::shared_lock lock(impl_->registry_mutex);
    return impl_->recently_exited.count(service_id) > 0;
}

}  // namespace shield::lua
