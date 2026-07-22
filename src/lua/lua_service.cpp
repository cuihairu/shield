// [SHIELD_LUA] Lua service implementation
#include "shield/lua/lua_service.hpp"

#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/mail_cache.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <condition_variable>
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
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_runtime.hpp"

namespace shield::lua {

namespace {

struct DispatchFrame {
    std::string service_id;
    std::string sender_id;
    std::string trace_id;
    int64_t deadline_ms = 0;
    bool in_exit = false;
    bool exit_requested = false;
    std::string exit_reason = "normal";
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

struct LuaServiceManager::Impl {
    LuaRuntime& runtime;
    caf::actor_system& system;
    std::unordered_map<std::string, std::shared_ptr<LuaVM>> services;
    std::unordered_map<std::string, std::string> published_names;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        owned_names;
    std::unordered_map<std::string, std::string> module_scripts;
    std::vector<std::string> service_order;
    mutable std::shared_mutex registry_mutex;
    std::atomic<bool> stopping{
        false};  // set by shutdown_all, checked by send/call/spawn

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
    struct PendingCall {
        uint64_t session = 0;
        lua_State* caller_co = nullptr;
        int caller_anchor = LUA_NOREF;  // registry ref keeping caller_co alive
        int64_t deadline_ms = 0;
        std::string caller_service;
    };
    std::atomic<uint64_t> next_call_session{1};
    std::unordered_map<uint64_t, PendingCall>
        pending_calls;  // session -> caller wait
    std::unordered_map<lua_State*, uint64_t>
        handler_call_session;  // callee co -> session

    // Per-service consecutive error counter for panic detection.
    // Reset on successful handler completion; incremented on uncaught error.
    std::unordered_map<std::string, int> error_counts;

    // Track recently exited services for service_dead error distinction.
    // Cleared periodically to avoid unbounded growth.
    std::unordered_set<std::string> recently_exited;

    // Track last sender per service for context_expired detection.
    std::unordered_map<std::string, std::string> last_sender_per_service;

    // Permission check hook (Phase 2). When set, called before send/call.
    // Returns empty string if allowed, error code if denied.
    std::function<std::string(const std::string& sender,
                              const std::string& target,
                              const std::string& method)>
        permission_check;

    // CAF actor system reference. LuaServiceManager now always requires a CAF
    // actor system; every spawned service owns a CAF actor handle.
    std::unordered_map<std::string, caf::actor> service_actors;

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
    std::atomic<uint64_t> next_sync_call_session{1};

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

    // Dispatch a CAF-native SyncCallMessage. Uses sync_session as the
    // call_session so on_handler_completed can signal the blocking caller.
    void dispatch_sync_call_message(class LuaServiceManager* manager,
                                    const std::string& id,
                                    const SyncCallMessage& req) {
        DispatchMessage msg;
        msg.sender = req.sender;
        msg.method = req.method;
        msg.args = req.args;
        msg.trace_id = req.trace_id;
        msg.deadline_ms = req.deadline_ms;
        msg.high_priority = false;
        msg.timestamp_ms = req.timestamp_ms;
        msg.call_session = req.sync_session;
        (void)dispatch_message(manager, id, msg);
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

        std::string exit_service_id;
        std::string exit_reason;
        if (is_exit_requested(&exit_service_id, &exit_reason) &&
            exit_service_id == id) {
            manager->exit(exit_service_id, exit_reason);
        }

        return true;
    }

    void run_fork_task_now(class LuaServiceManager* manager,
                           const ForkedTask& task) {
        DispatchScope scope(*this, task.service_id, "", false);
        if (task.raw_fn.valid()) {
            lua_State* L = task.raw_fn.lua_state();
            task.raw_fn.push(L);
            int status = lua_pcall(L, 0, 0, 0);
            if (status != LUA_OK) {
                std::string err = "fork error";
                if (lua_type(L, -1) == LUA_TSTRING) {
                    err = lua_tostring(L, -1);
                }
                lua_settop(L, 0);
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_ERROR(log, "forked task " + std::to_string(task.id) +
                                          " error: " + err);
                manager->invoke_error_hook(task.service_id, "fork", "", err);
            }
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
        lua_State* L = cb.lua_state();
        cb.push(L);
        int status = lua_pcall(L, 0, 0, 0);
        if (status != LUA_OK) {
            std::string err = "timer error";
            if (lua_type(L, -1) == LUA_TSTRING) {
                err = lua_tostring(L, -1);
            }
            lua_settop(L, 0);
            manager->invoke_error_hook(service_id, "timer", "", err);
        }
    }

    void fire_actor_timer(class LuaServiceManager* manager,
                          const std::string& service_id, uint64_t timer_id) {
        ActorTimerState timer;
        bool found = false;
        {
            std::unique_lock lock(registry_mutex);
            auto it = actor_timers.find(timer_id);
            if (it != actor_timers.end() && it->second.active) {
                timer = it->second;
                found = true;
                if (!it->second.repeating) {
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
            timer.native_callback();
        } else {
            run_timer_callback_now(manager, service_id, timer.raw_callback);
        }
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

    bool is_exit_requested(std::string* service_id, std::string* reason) const {
        if (tls_dispatch_stack.empty() ||
            !tls_dispatch_stack.back().exit_requested) {
            return false;
        }
        if (service_id) {
            *service_id = tls_dispatch_stack.back().service_id;
        }
        if (reason) {
            *reason = tls_dispatch_stack.back().exit_reason;
        }
        return true;
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
                false,
                "normal",
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
}

LuaServiceManager::~LuaServiceManager() {
    // Cancel pending timer/fork callbacks for every owned service
    // before this manager's state (and the service VMs it owns) is destroyed.
    // TimerManager lives in LuaRuntime, which outlives this manager; without
    // this cleanup its sol::function/std::function callbacks would be released
    // after the owning lua_State is already closed.
    std::vector<std::string> service_ids;
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
            cancel_actor_timer(timer_id);
        }
    }

    // Tear down any CAF service actors. The actor system (when attached)
    // outlives this manager because bootstrap resets it afterwards, so it is
    // safe to ask the actors to stop here.
    {
        std::unique_lock lock(impl_->registry_mutex);
        for (auto& [id, actor] : impl_->service_actors) {
            caf::anon_send_exit(actor, caf::exit_reason::user_shutdown);
        }
        impl_->service_actors.clear();
        for (auto& [session, driver] : impl_->actor_call_timeouts) {
            if (driver) {
                caf::anon_send_exit(driver, caf::exit_reason::user_shutdown);
            }
        }
        impl_->actor_call_timeouts.clear();
    }

    // Wake up any pending sync calls (manager->call() blocked on CV)
    // so they don't hang forever during shutdown.
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
            std::shared_lock lock(impl_->registry_mutex);
            if (impl_->services.contains(service_name)) {
                return SpawnResult::error("service already exists: " +
                                          service_name);
            }
            if (impl_->published_names.contains(service_name)) {
                return SpawnResult::error("service name already exists: " +
                                          service_name);
            }
        }
        if (!Impl::valid_name(service_name)) {
            return SpawnResult::error("invalid service name: " + service_name);
        }

        const std::string script_path = impl_->resolve_module(module);

        // Create VM and load module
        auto vm = impl_->runtime.create_vm();

        std::string error;
        if (!impl_->runtime.register_api(vm, &error)) {
            return SpawnResult::error("Failed to register Lua API: " + error);
        }
        if (!impl_->runtime.load_service_module(vm, script_path, &error)) {
            return SpawnResult::error("Failed to load module: " + script_path +
                                      ": " + error);
        }

        // Parse spawn timeout (default 10s).
        const int64_t spawn_timeout_ms = opts.value("timeout", 10000);

        // Create a CAF actor for this service before on_init so that
        // on_init-time fork/timer registration can immediately route through
        // the actor path. The service VM is published only after on_init
        // succeeds; until then the actor exists purely as an internal handle.
        //
        // The behavior pattern-matches the native typed messages
        // (ServiceMessage, SyncCallMessage, timer_fire_atom + uint64_t,
        // call_timeout_atom + uint64_t). No string/JSON dispatch remains.
        auto actor = impl_->system.spawn([impl_ptr = impl_.get(),
                                          manager = this, svc = service_name](
                                             caf::event_based_actor* self)
                                             -> caf::behavior {
            // Message stashing: until on_init completes on the spawning
            // thread, every incoming message is stashed so fork/timer/
            // call cannot touch this Lua VM concurrently with on_init.
            // The spawner sends init_ready_atom once on_init returns;
            // we then install the real behavior and release the stash.
            auto cache = std::make_shared<caf::mail_cache>(self, 4096);
            self->set_default_handler(
                [cache](caf::message& msg) -> caf::skippable_result {
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
                        [impl_ptr, manager, svc](const SyncCallMessage& req) {
                            impl_ptr->dispatch_sync_call_message(manager, svc,
                                                                 req);
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
                    });
                    cache->unstash();
                },
            };
        });
        {
            std::unique_lock lock(impl_->registry_mutex);
            impl_->service_actors.emplace(service_name, actor);
        }

        nlohmann::json init_args = {
            {"name", service_name},
            {"id", service_name},
            {"args", args},
            {"config", config},
        };
        bool exit_after_init = false;
        std::string exit_reason;
        {
            const auto init_start = std::chrono::steady_clock::now();

            Impl::DispatchScope scope(*impl_, service_name, "", false);
            if (!impl_->runtime.call_service_function(vm, "on_init", init_args,
                                                      &error)) {
                // Check if failure was due to timeout.
                const auto init_end = std::chrono::steady_clock::now();
                const auto init_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        init_end - init_start)
                        .count();
                if (init_ms >= spawn_timeout_ms) {
                    return SpawnResult::error(
                        "spawn timeout: on_init took " +
                        std::to_string(init_ms) + "ms (limit " +
                        std::to_string(spawn_timeout_ms) + "ms)");
                }
                {
                    std::unique_lock lock(impl_->registry_mutex);
                    impl_->service_actors.erase(service_name);
                    if (auto names_it = impl_->owned_names.find(service_name);
                        names_it != impl_->owned_names.end()) {
                        for (const auto& name : names_it->second) {
                            impl_->published_names.erase(name);
                        }
                        impl_->owned_names.erase(names_it);
                    }
                }
                return SpawnResult::error("on_init failed for " + service_name +
                                          ": " + error);
            }
            std::string exit_service_id;
            exit_after_init =
                impl_->is_exit_requested(&exit_service_id, &exit_reason) &&
                exit_service_id == service_name;
        }

        {
            std::unique_lock lock(impl_->registry_mutex);
            if (impl_->services.contains(service_name) ||
                impl_->published_names.contains(service_name)) {
                impl_->service_actors.erase(service_name);
                return SpawnResult::error("service name already exists: " +
                                          service_name);
            }
            impl_->services[service_name] = std::move(vm);
            impl_->published_names[service_name] = service_name;
            impl_->owned_names[service_name].insert(service_name);
            impl_->service_order.push_back(service_name);
        }

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
    if (impl_->permission_check) {
        const std::string sender = current_service_id();
        const std::string denial = impl_->permission_check(
            sender, std::string(target), std::string(method));
        if (!denial.empty()) {
            if (error) *error = "permission denied: " + denial;
            return false;
        }
    }

    if (!validate_message_payload(args, error)) {
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
        if (error) {
            if (impl_->recently_exited.count(service_id) > 0 ||
                impl_->recently_exited.count(std::string(target)) > 0) {
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

    // Create pending sync call.
    const uint64_t session = impl_->next_sync_call_session.fetch_add(1);
    auto pending = std::make_shared<Impl::PendingSyncCall>();
    pending->session = session;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls[session] = pending;
    }

    // Build typed SyncCallMessage for CAF-native dispatch.
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    SyncCallMessage sync_msg;
    sync_msg.sync_session = session;
    sync_msg.sender = sender;
    sync_msg.method = std::string(method);
    sync_msg.args = args;
    sync_msg.trace_id = impl_->current_trace_id();
    sync_msg.deadline_ms = impl_->current_deadline_ms();
    sync_msg.timestamp_ms = now_ms;

    // Send to target actor.
    caf::anon_send(*actor_opt, std::move(sync_msg));

    // Block until handler completes (or timeout).
    const int32_t effective_timeout = timeout_ms > 0 ? timeout_ms : 5000;
    bool completed = false;
    {
        std::unique_lock lk(pending->mtx);
        completed = pending->cv.wait_for(
            lk, std::chrono::milliseconds(effective_timeout),
            [&] { return pending->completed; });
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

    std::string error;
    nlohmann::json args = std::string(reason);
    Impl::DispatchScope scope(*impl_, id, "", true);
    (void)impl_->runtime.call_service_function(service, "on_exit", args,
                                               &error);

    // Cancel forked tasks / timers / coroutines BEFORE erasing the service
    // VM. These hold sol::function / std::function callbacks that reference
    // the service's lua_State; releasing them after the VM is destroyed
    // would luaL_unref on a closed state.
    cancel_forked_tasks_for_service(id);
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
        cancel_actor_timer(timer_id);
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
                    caf::anon_send_exit(it->second,
                                        caf::exit_reason::user_shutdown);
                }
                it = impl_->actor_call_timeouts.erase(it);
            } else {
                ++it;
            }
        }
    }

    {
        std::unique_lock lock(impl_->registry_mutex);
        if (auto names_it = impl_->owned_names.find(id);
            names_it != impl_->owned_names.end()) {
            for (const auto& name : names_it->second) {
                impl_->published_names.erase(name);
            }
            impl_->owned_names.erase(names_it);
        }
        impl_->services.erase(id);
        impl_->service_order.erase(std::remove(impl_->service_order.begin(),
                                               impl_->service_order.end(), id),
                                   impl_->service_order.end());
        impl_->recently_exited.insert(id);

        // Tear down the service's CAF actor. anon_send_exit asks the actor to
        // stop; erasing the handle releases our reference.
        if (auto actor_it = impl_->service_actors.find(id);
            actor_it != impl_->service_actors.end()) {
            caf::anon_send_exit(actor_it->second,
                                caf::exit_reason::user_shutdown);
            impl_->service_actors.erase(actor_it);
        }
    }
}

void LuaServiceManager::shutdown_all(std::string_view reason) {
    impl_->stopping.store(true);
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
            exit(*it, reason);
        }
    }
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->service_order.clear();
    }
}

void LuaServiceManager::stop_worker() {
    // No-op: worker thread was removed during CAF migration.
    // Kept for backward compatibility with bootstrap.cpp.
}

void LuaServiceManager::attach_actor_system(caf::actor_system& system) {
    // No-op: actor system is now injected via constructor.
    // Kept for backward compatibility with bootstrap.cpp.
}

std::string LuaServiceManager::current_service_id() const {
    return impl_->current_service_id();
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
    frame.exit_requested = true;
    frame.exit_reason = reason.empty() ? "normal" : std::string(reason);
}

bool LuaServiceManager::is_in_exit() const {
    if (tls_dispatch_stack.empty()) {
        return false;
    }
    return tls_dispatch_stack.back().in_exit;
}

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
    return true;
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
    if (service_id.empty()) {
        return 0;
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
    impl_->pending_calls.emplace(session, std::move(pc));

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
        impl_->handler_call_session[co] = session;
    }
}

void LuaServiceManager::on_handler_completed(
    lua_State* co, const nlohmann::json& return_values) {
    if (co == nullptr) {
        return;
    }
    auto it = impl_->handler_call_session.find(co);
    if (it == impl_->handler_call_session.end()) {
        return;
    }
    const uint64_t session = it->second;
    impl_->handler_call_session.erase(it);
    resume_caller(session, true, return_values);
}

void LuaServiceManager::resume_caller(uint64_t session, bool ok,
                                      const nlohmann::json& values) {
    // Step 3: check if this is a sync_call session (from manager->call()
    // routed through CAF). If so, signal the blocking CV instead of resuming
    // a Lua coroutine.
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto sync_it = impl_->pending_sync_calls.find(session);
        if (sync_it != impl_->pending_sync_calls.end()) {
            auto pending = sync_it->second;
            impl_->pending_sync_calls.erase(sync_it);
            lock.unlock();
            {
                std::unique_lock lk(pending->mtx);
                pending->ok = ok;
                pending->values = values;
                pending->completed = true;
            }
            pending->cv.notify_one();
            return;
        }
    }

    // Existing: coro_call path — resume the caller's Lua coroutine.
    auto it = impl_->pending_calls.find(session);
    if (it == impl_->pending_calls.end()) {
        return;
    }
    Impl::PendingCall pc = std::move(it->second);
    impl_->pending_calls.erase(it);

    // Cancel the CAF call-timeout driver (if any) now that the call has
    // completed normally. The timeout path itself erases the driver before
    // calling resume_caller, so this is a no-op for the timeout branch.
    cancel_actor_call_timeout(session);

    lua_State* caller_co = pc.caller_co;
    if (caller_co == nullptr) {
        return;
    }

    // Build the resume payload: (ok, values...). The caller's shield.call
    // wrapper unpacks these via coroutine.yield()'s return values.
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
    if (status != LUA_OK && status != LUA_YIELD) {
        // Caller errored resuming; drop it silently (logged elsewhere).
        lua_settop(caller_co, 0);
    }
    // Release the anchor; the coroutine either completed or re-yielded (in
    // which case a subsequent suspend re-anchored it).
    if (pc.caller_anchor != LUA_NOREF) {
        luaL_unref(caller_co, LUA_REGISTRYINDEX, pc.caller_anchor);
    }
}

int LuaServiceManager::check_call_timeouts(int64_t now_ms) {
    // Collect expired sessions first; we must not modify pending_calls while
    // iterating, and resume_caller erases from it.
    std::vector<uint64_t> expired;
    for (const auto& [session, pc] : impl_->pending_calls) {
        if (pc.deadline_ms <= now_ms) {
            expired.push_back(session);
        }
    }

    nlohmann::json timeout_err = nlohmann::json::array(
        {nlohmann::json::object({{"code", "timeout"},
                                 {"message", "call timeout"},
                                 {"retryable", true}})});

    for (uint64_t session : expired) {
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

    // Increment error counter.
    int& count = impl_->error_counts[service_id];
    ++count;

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
    std::unique_lock lock(impl_->registry_mutex);
    auto it = impl_->actor_timers.find(id);
    if (it == impl_->actor_timers.end()) {
        return false;
    }
    if (it->second.driver) {
        caf::anon_send_exit(it->second.driver, caf::exit_reason::user_shutdown);
    }
    auto by_service_it =
        impl_->actor_timers_by_service.find(it->second.service_id);
    if (by_service_it != impl_->actor_timers_by_service.end()) {
        by_service_it->second.erase(id);
        if (by_service_it->second.empty()) {
            impl_->actor_timers_by_service.erase(by_service_it);
        }
    }
    impl_->actor_timers.erase(it);
    return true;
}

size_t LuaServiceManager::active_actor_timer_count() const {
    std::shared_lock lock(impl_->registry_mutex);
    return impl_->actor_timers.size();
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
    std::unique_lock lock(impl_->registry_mutex);
    auto it = impl_->actor_call_timeouts.find(session);
    if (it == impl_->actor_call_timeouts.end()) {
        return false;
    }
    if (it->second) {
        caf::anon_send_exit(it->second, caf::exit_reason::user_shutdown);
    }
    impl_->actor_call_timeouts.erase(it);
    return true;
}

}  // namespace shield::lua
