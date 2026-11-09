// [SHIELD_LUA] Lua service timers, forked tasks and gateway actors:
// driver actors, fire/cancel plumbing and the fork task queue.
// Split out of lua_service.cpp; member declarations live in
// lua_service_impl.hpp.
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

// Re-anchor a sol reference onto the main thread's lua_State. Handlers
// registered from inside a coroutine (on_init and every service handler run
// as one) carry that coroutine's lua_State, which the GC may collect once
// the coroutine finishes; a stored lua_State* would then dangle and any
// later use (luaL_unref from the cancel/erase paths) would crash. The
// registry is shared across all threads of one global_State, so re-refering
// the same registry entry from the main thread keeps the stored pointer
// alive without changing which value the reference names.
shd::function anchor_to_main_thread(shd::function fn) {
    lua_State* state = fn.lua_state();
    if (state == nullptr ||
        !fn.valid()) {  // GCOVR_EXCL_BR_LINE (defensive: register/fork call
                        // sites always pass live handler refs; an empty ref
                        // never reaches this guard)
        return fn;
    }
    lua_State* main_state = shd::main_thread(state);
    if (main_state ==   // GCOVR_EXCL_BR_LINE
            nullptr ||  // GCOVR_EXCL_BR_LINE (defensive: null main-thread arm
                        // (handlers register from coroutines))
        main_state ==
            state) {  // GCOVR_EXCL_BR_LINE (defensive: handlers register from
                      // coroutines, never the main thread; the already-anchored
                      // arm cannot occur)
        return fn;
    }
    return shd::function(main_state, shd::ref_index(fn.registry_index()));
}

void LuaServiceManager::Impl::run_fork_task_now(
    class LuaServiceManager* manager, const ForkedTask& task) {
    // Pairs with the fetch_add in run_ready_fork_task's critical section:
    // the drop happens only after this body — which may call the
    // task's Lua function on the registering VM — has fully returned.
    struct ActiveForkTaskGuard {
        std::atomic<std::size_t>& count;
        ~ActiveForkTaskGuard() { count.fetch_sub(1); }
    } active_guard{active_fork_tasks};
    DispatchScope scope(*this, task.service_id, "", false);
    if (task.raw_fn.valid()) {
        // Coroutine-aware dispatch: the forked task may yield via
        // shield.sleep / shield.call; the actor thread returns to the
        // mailbox immediately and the suspended coroutine is resumed by
        // the runtime when its wait completes.
        std::string error;
        std::shared_ptr<LuaVM> service = find_dispatch_vm(task.service_id);
        if (service &&  // GCOVR_EXCL_BR_LINE (race: a stale queued task
                        // whose service vanished -
                        // cancel_forked_tasks_for_service drains before
                        // teardown)
            runtime.invoke_coroutine(  // GCOVR_EXCL_BR_LINE (compiler
                                       // artifact: multi-line call
                                       // continuation arcs)
                service, task.raw_fn, {},
                "fork",  // GCOVR_EXCL_BR_LINE (compiler artifact:
                         // multi-line call continuation arcs)
                "",      // GCOVR_EXCL_BR_LINE (compiler artifact: multi-line
                         // call continuation arcs)
                0, manager, task.service_id, &error)) {
            return;
        }
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(
            log, "forked task " + std::to_string(task.id) + " error: " + error);
    } else {
        try {
            task.fn();
        } catch (                  // GCOVR_EXCL_BR_LINE
            const std::exception&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // catch structural edge (landing edge is
                                   // covered by tests))
                e) {               // GCOVR_EXCL_BR_LINE
                                   // (defensive: fork task bodies
                                   // (runtime glue and tests) do
                                   // not throw; the guard is
                                   // contractual)
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_ERROR(log, "forked task " + std::to_string(task.id) +
                                      " error: " + e.what());
            manager->invoke_error_hook(task.service_id, "fork", "", e.what());
        }
    }
}

void LuaServiceManager::Impl::run_ready_fork_task(
    class LuaServiceManager* manager, uint64_t task_id) {
    std::optional<ForkedTask> task;
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (auto it = pending_tasks.begin(); it != pending_tasks.end(); ++it) {
            if (it->id ==
                task_id) {  // GCOVR_EXCL_BR_LINE (race: ready-fired id
                            // already canceled - driver-vs-cancel window)
                task = std::move(*it);
                // Same critical section as the erase below: a waiter that
                // observes pending_task_count_total() == 0 is therefore
                // guaranteed to also observe this increment, so draining
                // on both counters has no dequeue/execute gap.
                active_fork_tasks.fetch_add(1);
                pending_tasks.erase(it);
                break;
            }
        }
        if (task) {
            auto by_service_it = tasks_by_service.find(task->service_id);
            if (by_service_it !=
                tasks_by_service.end()) {  // GCOVR_EXCL_BR_LINE (defensive: the
                                           // by_service index is inserted
                                           // atomically with the task push; the
                                           // entry always exists)
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

void LuaServiceManager::Impl::run_timer_callback_now(
    class LuaServiceManager* manager, const std::string& service_id,
    shd::function cb) {
    if (!cb.valid()) {  // GCOVR_EXCL_BR_LINE (defensive: branch of a line
                        // already excluded (GCOVR_EXCL_LINE defensive
                        // callback guard))
        return;         // GCOVR_EXCL_LINE (callback is valid at schedule time)
    }
    DispatchScope scope(*this, service_id, "", false);
    // Coroutine-aware dispatch: the callback may yield via shield.sleep /
    // shield.call; the actor thread returns to the mailbox immediately.
    std::string error;
    std::shared_ptr<LuaVM> service = find_dispatch_vm(service_id);
    if (!service) {  // GCOVR_EXCL_BR_LINE (defensive: branch of a line
                     // already excluded (GCOVR_EXCL_LINE teardown race
                     // guard))
        return;      // GCOVR_EXCL_LINE (race: timer fired after teardown)
    }
    if (!runtime.invoke_coroutine(service, cb, {}, "timer", "", 0, manager,
                                  service_id, &error)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(log, "timer error: " + error);
    }
}

void LuaServiceManager::Impl::fire_actor_timer(class LuaServiceManager* manager,
                                               const std::string& service_id,
                                               uint64_t timer_id) {
    ActorTimerState timer;
    caf::actor retired_driver;
    bool found = false;
    {
        std::unique_lock lock(registry_mutex);
        auto it = actor_timers.find(timer_id);
        if (it != actor_timers.end() &&
            it->second.active) {  // GCOVR_EXCL_BR_LINE (defensive: active is
                                  // only cleared together with the map erase; a
                                  // false entry is never observable)
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
                actor_timers_by_service[it->second.service_id].erase(timer_id);
                actor_timers.erase(it);
            } else {
                // The driver reschedules itself; keep the projection's
                // due time in step (drift equals the fire-dispatch lag).
                it->second.next_fire_ms =
                    Impl::now_ms() + it->second.interval_ms;
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
        wait_for_actors(        // GCOVR_EXCL_BR_LINE (compiler artifact: braced
                                // single-element init arc)
            {retired_driver});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // braced single-element init arc)
        retire_actor(std::move(retired_driver));
    }
}

bool LuaServiceManager::Impl::collect_actor_timer_for_cancel(
    uint64_t id, std::vector<caf::actor>* actors_to_stop) {
    std::unique_lock lock(registry_mutex);
    auto it = actor_timers.find(id);
    if (it == actor_timers.end()) {
        return false;
    }
    if (it->second.driver) {  // GCOVR_EXCL_BR_LINE (defensive: timer
                              // entries always carry their driver; fired or
                              // canceled timers erase the entry)
        // Move the driver out: the entry dies below while the driver
        // actor may still be running its tick handler. Releasing its last
        // external reference at that moment would trigger CAF's
        // on_unreachable teardown on this thread concurrently with that
        // handler; the reference must only be released after
        // stop_and_wait_for_actors joined the driver.
        actors_to_stop->push_back(std::move(it->second.driver));
    }
    auto by_service_it = actor_timers_by_service.find(it->second.service_id);
    if (by_service_it !=
        actor_timers_by_service
            .end()) {  // GCOVR_EXCL_BR_LINE (defensive: the by_service
                       // index is inserted when the timer is scheduled; the
                       // entry always exists)
        by_service_it->second.erase(id);
        if (by_service_it->second.empty()) {
            actor_timers_by_service.erase(by_service_it);
        }
    }
    actor_timers.erase(it);
    return true;
}

uint64_t LuaServiceManager::enqueue_forked_task(std::string service_id,
                                                std::function<void()> task) {
    return enqueue_forked_task(std::move(service_id), std::move(task),
                               shd::function{});
}

uint64_t LuaServiceManager::enqueue_forked_task(std::string service_id,
                                                std::function<void()> task,
                                                shd::function raw_fn) {
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
        service_id = impl_->service_actors.begin()
                         ->first;  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // straight-line iterator dereference; gcov
                                   // synthesizes arcs)
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
    // is looked up by id in pending_tasks, so the shd::function never crosses
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
        // Same abandon-before-erase as cancel_forked_tasks_for_service:
        // this thread may not be the VM's owner, so the anchored function
        // must not luaL_unref the registry here. The leaked entry dies with
        // the VM.
        for (auto& t : impl_->pending_tasks) {
            if (t.id == id) {
                t.raw_fn.abandon();
            }
        }
        auto by_service_it = impl_->tasks_by_service.find(service_id);
        if (by_service_it !=
            impl_->tasks_by_service
                .end()) {  // GCOVR_EXCL_BR_LINE (defensive: the by_service
                           // index is inserted atomically with the task push;
                           // the entry always exists)
            by_service_it->second.erase(id);
            if (by_service_it  // GCOVR_EXCL_BR_LINE
                    ->second   // GCOVR_EXCL_BR_LINE (race: rollback racing the
                               // exit drain leaves sibling entries)
                    .empty()) {  // GCOVR_EXCL_BR_LINE (defensive: a rollback
                                 // erases the only entry the failed enqueue
                                 // added; siblings are drained with the actor)
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
        // Dropping a queued task releases its anchored Lua function, and
        // this may run on any thread (manager teardown, service exit). The
        // sol destructor would luaL_unref the owning VM's registry — a
        // cross-thread Lua mutation racing the service actor's own dispatch
        // on the same registry. Abandon the ref instead: the registry entry
        // leaks and is reclaimed when the VM closes (same policy as the
        // dropped spawn jobs above).
        for (auto& t : impl_->pending_tasks) {
            if (ids.count(t.id) > 0) {
                t.raw_fn.abandon();
            }
        }
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

size_t LuaServiceManager::active_fork_task_count() const {
    return impl_->active_fork_tasks.load();
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

uint64_t LuaServiceManager::schedule_actor_timer_once(
    int64_t delay_ms, shd::function callback, const std::string& service_id) {
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
        id = impl_->next_actor_timer_id.fetch_add(1);  // GCOVR_EXCL_BR_START
        impl_->actor_timers[id] = LuaServiceManager::Impl::ActorTimerState{
            //  (compiler artifact: ActorTimerState
            // designated-init aggregation arcs)
            //  (compiler artifact: ActorTimerState
            // designated-init aggregation arcs)
            .id = id,
            .interval_ms = delay_ms,
            .repeating = false,
            .next_fire_ms = Impl::now_ms() + delay_ms,
            .service_id = service_id,
            .raw_callback = callback,
            .native_callback = {},
            .has_native_callback = false,
            .active = true,
            .driver = caf::actor{}};  // GCOVR_EXCL_BR_STOP
    }

    try {
        auto driver = impl_->system.spawn(
            [svc = service_id,  // GCOVR_EXCL_BR_LINE
             tid = id,  // GCOVR_EXCL_BR_LINE (compiler artifact: driver lambda
                        // entry/exit arcs)
             target = *service_actor,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // driver lambda entry/exit arcs)
             delay_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(delay_ms),
                                   caf::tick_atom_v);
                return caf::behavior{
                    [=](caf::tick_atom) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: inner tick lambda arcs)
                        caf::anon_send(target, timer_fire_atom_v, tid);
                        self->quit();
                    }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers
                      .end()) {  // GCOVR_EXCL_BR_LINE (defensive: same-thread
                                 // window between insert and driver arm; erase
                                 // sites hold the registry lock)
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (defensive: untestable
                                    // actor-spawn failure (see the exclusion
                                    // region at 4877))
                 e) {               // GCOVR_EXCL_START (untestable
                                    // actor-spawn failure)
        // Clean up the timer state if spawn fails
        std::unique_lock lock(
            impl_->registry_mutex);  //  (defensive:
                                     // untestable actor-spawn failure (see
                                     // the exclusion region at 4877))
        impl_->actor_timers.erase(   //  (defensive: untestable actor-spawn
                                    // failure (see the exclusion region below))
            id);  //  (defensive: untestable actor-spawn
                  // failure (see the exclusion region at 4877))
        auto& log = shield::log::get_logger(  //  (defensive:
                                              // untestable actor-spawn failure
                                              // (see the exclusion region
                                              // below))
            "lua");                           //  (defensive: untestable
                     // actor-spawn failure (see the exclusion region at 4877))
        SHIELD_LOG_ERROR(  //  (defensive: untestable
                           // actor-spawn failure (see the exclusion region at
                           // 4877))
            log, std::string("Failed to spawn timer actor: ") + e.what());
        return 0;
    }
    // GCOVR_EXCL_STOP
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
        id = impl_->next_actor_timer_id.fetch_add(1);  // GCOVR_EXCL_BR_START
        impl_->actor_timers[id] = LuaServiceManager::Impl::ActorTimerState{
            //  (compiler artifact: ActorTimerState
            // designated-init aggregation arcs)
            //  (compiler artifact: ActorTimerState
            // designated-init aggregation arcs)
            .id = id,
            .interval_ms = delay_ms,
            .repeating = false,
            .next_fire_ms = Impl::now_ms() + delay_ms,
            .service_id = service_id,
            .raw_callback = shd::function{},
            .native_callback = std::move(callback),
            .has_native_callback = true,
            .active = true,
            .driver = caf::actor{}};  // GCOVR_EXCL_BR_STOP
    }

    try {
        auto driver = impl_->system.spawn(
            [svc = service_id,  // GCOVR_EXCL_BR_LINE
             tid = id,  // GCOVR_EXCL_BR_LINE (compiler artifact: driver lambda
                        // entry/exit arcs)
             target = *service_actor,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // driver lambda entry/exit arcs)
             delay_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(delay_ms),
                                   caf::tick_atom_v);
                return caf::behavior{
                    [=](caf::tick_atom) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: inner tick lambda arcs)
                        caf::anon_send(target, timer_fire_atom_v, tid);
                        self->quit();
                    }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers
                      .end()) {  // GCOVR_EXCL_BR_LINE (defensive: same-thread
                                 // window between insert and driver arm; erase
                                 // sites hold the registry lock)
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (defensive: untestable
                                    // actor-spawn failure (see the exclusion
                                    // region at 4934))
                 e) {               // GCOVR_EXCL_START (untestable
                                    // actor-spawn failure)
        // Clean up the timer state if spawn fails
        std::unique_lock lock(
            impl_->registry_mutex);  //  (defensive:
                                     // untestable actor-spawn failure (see
                                     // the exclusion region at 4934))
        impl_->actor_timers.erase(   //  (defensive: untestable actor-spawn
                                    // failure (see the exclusion region below))
            id);  //  (defensive: untestable actor-spawn
                  // failure (see the exclusion region at 4934))
        auto& log = shield::log::get_logger(  //  (defensive:
                                              // untestable actor-spawn failure
                                              // (see the exclusion region
                                              // below))
            "lua");                           //  (defensive: untestable
                     // actor-spawn failure (see the exclusion region at 4934))
        SHIELD_LOG_ERROR(  //  (defensive: untestable
                           // actor-spawn failure (see the exclusion region at
                           // 4934))
            log, std::string("Failed to spawn timer actor: ") + e.what());
        return 0;
    }
    // GCOVR_EXCL_STOP
    return id;
}

uint64_t LuaServiceManager::schedule_actor_timer_fixed_delay(
    int64_t interval_ms, shd::function callback,
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
        id = impl_->next_actor_timer_id.fetch_add(1);  // GCOVR_EXCL_BR_START
        impl_->actor_timers[id] = LuaServiceManager::Impl::ActorTimerState{
            //  (compiler artifact: ActorTimerState
            // designated-init aggregation arcs)
            //  (compiler artifact: ActorTimerState
            // designated-init aggregation arcs)
            .id = id,
            .interval_ms = interval_ms,
            .repeating = true,
            .next_fire_ms = Impl::now_ms() + interval_ms,
            .service_id = service_id,
            .raw_callback = callback,
            .native_callback = {},
            .has_native_callback = false,
            .active = true,
            .driver = caf::actor{}};  // GCOVR_EXCL_BR_STOP
    }

    try {
        auto driver = impl_->system.spawn(
            [svc = service_id,  // GCOVR_EXCL_BR_LINE
             tid = id,  // GCOVR_EXCL_BR_LINE (compiler artifact: driver lambda
                        // entry/exit arcs)
             target = *service_actor,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // driver lambda entry/exit arcs)
             interval_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(interval_ms),
                                   caf::tick_atom_v);
                return caf::behavior{
                    [=](caf::tick_atom) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: inner tick lambda arcs)
                        caf::anon_send(target, timer_fire_atom_v, tid);
                        self->delayed_send(
                            self, std::chrono::milliseconds(interval_ms),
                            caf::tick_atom_v);
                    }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers
                      .end()) {  // GCOVR_EXCL_BR_LINE (defensive: same-thread
                                 // window between insert and driver arm; erase
                                 // sites hold the registry lock)
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (defensive: untestable
                                    // actor-spawn failure (see the exclusion
                                    // region at 4999))
                 e) {               // GCOVR_EXCL_START (untestable
                                    // actor-spawn failure)
        // Clean up the timer state if spawn fails
        std::unique_lock lock(
            impl_->registry_mutex);  //  (defensive:
                                     // untestable actor-spawn failure (see
                                     // the exclusion region at 4999))
        impl_->actor_timers.erase(   //  (defensive: untestable actor-spawn
                                    // failure (see the exclusion region below))
            id);  //  (defensive: untestable actor-spawn
                  // failure (see the exclusion region at 4999))
        auto& log = shield::log::get_logger(  //  (defensive:
                                              // untestable actor-spawn failure
                                              // (see the exclusion region
                                              // below))
            "lua");                           //  (defensive: untestable
                     // actor-spawn failure (see the exclusion region at 4999))
        SHIELD_LOG_ERROR(  //  (defensive: untestable
                           // actor-spawn failure (see the exclusion region at
                           // 4999))
            log, std::string("Failed to spawn timer actor: ") + e.what());
        return 0;
    }
    // GCOVR_EXCL_STOP
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

}  // namespace shield::lua
