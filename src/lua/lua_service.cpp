// [SHIELD_LUA] Lua service implementation
#include "shield/lua/lua_service.hpp"

#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>
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
#include "shield/log/logger.hpp"
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
    std::unordered_map<std::string, std::shared_ptr<LuaVM>> services;
    std::unordered_map<std::string, std::string> published_names;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        owned_names;
    std::unordered_map<std::string, std::string> module_scripts;
    std::unordered_map<std::string, std::shared_ptr<Mailbox>> mailboxes;
    std::vector<std::string> service_order;
    mutable std::shared_mutex registry_mutex;
    std::atomic<bool> stopping{
        false};  // set by shutdown_all, checked by send/call/spawn

    // Forked task queue. Drained only by the worker thread (or pump_once on
    // the caller's thread when no worker is running).
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
    static constexpr int kDefaultMaxErrorsBeforePanic = 10;

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

    // Worker thread lifecycle. The worker drives mailboxes, timers, forked
    // tasks, and coroutine timeouts. While the worker is running, all Lua
    // execution happens on the worker thread (modulo shield.call reentry,
    // which stays inline because it's already on the worker).
    std::atomic<bool> worker_running{false};
    std::atomic<bool> worker_stop_requested{false};
    std::thread worker_thread;
    std::mutex worker_mutex;
    std::condition_variable worker_cv;

    // CAF actor system bridge (optional). When attached, every spawned Lua
    // service also gets a CAF actor handle in service_actors. The actor is a
    // lifecycle-owned placeholder for now: it accepts fire-and-forget string
    // messages but does not yet drive Lua dispatch. Message delivery still
    // goes through the mailbox; the actor becomes the message entry point in
    // a later step. Protected by registry_mutex alongside the tables above.
    caf::actor_system* actor_system = nullptr;
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

    bool uses_caf_actor_system() const { return actor_system != nullptr; }

    int64_t clock_now_ms() const {
        std::shared_lock lock(registry_mutex);
        return clock_->now_ms();
    }

    int64_t clock_now_seconds() const {
        std::shared_lock lock(registry_mutex);
        return clock_->now_seconds();
    }

    // Decode a JSON-serialized message and dispatch it immediately on the
    // current actor thread. This bypasses the legacy mailbox pump for normal
    // message delivery when a CAF actor system is attached.
    void dispatch_json_message(class LuaServiceManager* manager,
                               const std::string& sid,
                               const std::string& json_str) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(json_str);
        } catch (...) {
            return;  // malformed payload; drop
        }

        const auto kind = j.value("kind", std::string("message"));
        if (kind == "fork") {
            run_ready_fork_task(manager, j.value("task_id", uint64_t(0)));
            return;
        }
        if (kind == "timer") {
            fire_actor_timer(manager, sid, j.value("timer_id", uint64_t(0)));
            return;
        }
        if (kind == "call_timeout") {
            const uint64_t session = j.value("session", uint64_t(0));
            // Clear the driver handle first so resume_caller (via the timeout
            // path) does not try to cancel a driver that is already done.
            manager->cancel_actor_call_timeout(session);
            nlohmann::json timeout_err = nlohmann::json::array(
                {nlohmann::json::object({{"code", "timeout"},
                                         {"message", "call timeout"},
                                         {"retryable", true}})});
            manager->resume_caller(session, false, timeout_err);
            return;
        }
        if (kind == "sync_call") {
            // Step 3: synchronous call routed through CAF actor. The caller
            // (main thread) is blocking on a PendingSyncCall CV. Dispatch the
            // method and, when the handler completes, signal the caller.
            const uint64_t sync_session = j.value("sync_session", uint64_t(0));
            const std::string method = j.value("method", std::string(""));
            const nlohmann::json args =
                j.value("args", nlohmann::json::array());
            const std::string sender = j.value("sender", std::string(""));
            Mailbox::Message msg;
            msg.sender = sender;
            msg.method = method;
            msg.args = args;
            msg.trace_id = j.value("trace_id", std::string(""));
            msg.deadline_ms = j.value("deadline_ms", int64_t(0));
            msg.priority = Mailbox::Priority::Normal;
            msg.timestamp_ms = j.value("timestamp_ms", int64_t(0));
            // Use sync_session as call_session so on_handler_completed
            // can identify it and signal the pending sync call.
            msg.call_session = sync_session;
            msg.call_reply_to = sender;
            (void)dispatch_message(manager, sid, msg);
            // If the handler completed synchronously (no yield),
            // on_handler_completed already signaled the CV. If it yielded
            // (e.g. shield.sleep), the handler will be resumed later and
            // on_handler_completed will signal then. Either way, the caller
            // is blocking on the CV and will be woken up.
            return;
        }

        Mailbox::Message msg;
        msg.sender = j.value("sender", std::string(""));
        msg.method = j.value("method", std::string(""));
        msg.args = j.value("args", nlohmann::json::object());
        msg.trace_id = j.value("trace_id", std::string(""));
        msg.deadline_ms = j.value("deadline_ms", int64_t(0));
        msg.priority = j.value("priority", 0u) == 1 ? Mailbox::Priority::High
                                                    : Mailbox::Priority::Normal;
        msg.timestamp_ms = j.value("timestamp_ms", int64_t(0));
        msg.call_session = j.value("call_session", uint64_t(0));
        msg.call_reply_to = j.value("call_reply_to", std::string(""));
        (void)dispatch_message(manager, sid, msg);
    }

    // Serialize a message into a JSON string for CAF transport. Used by the
    // send/send_system/send_call_request paths when a CAF actor is available.
    static std::string serialize_message(
        const std::string& sender, const std::string& method,
        const nlohmann::json& args, const std::string& trace_id,
        int64_t deadline_ms, uint32_t priority, int64_t timestamp_ms,
        uint64_t call_session = 0, const std::string& call_reply_to = "") {
        nlohmann::json j;
        j["sender"] = sender;
        j["method"] = method;
        j["args"] = args;
        j["trace_id"] = trace_id;
        j["deadline_ms"] = deadline_ms;
        j["priority"] = priority;
        j["timestamp_ms"] = timestamp_ms;
        j["call_session"] = call_session;
        j["call_reply_to"] = call_reply_to;
        return j.dump();
    }

    bool dispatch_message(class LuaServiceManager* manager,
                          const std::string& id, const Mailbox::Message& msg) {
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
            std::lock_guard<std::mutex> lock(worker_mutex);
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

    Impl(LuaRuntime& rt) : runtime(rt) {
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

LuaServiceManager::LuaServiceManager(LuaRuntime& runtime)
    : impl_(std::make_unique<Impl>(runtime)) {
    runtime.set_service_manager(this);
}

LuaServiceManager::~LuaServiceManager() {
    // Cancel pending timer/fork/coroutine callbacks for every owned service
    // before this manager's state (and the service VMs it owns) is destroyed.
    // TimerManager and CoroutineScheduler live in LuaRuntime, which outlives
    // this manager; without this cleanup their sol::function/std::function
    // callbacks would be released after the owning lua_State is already closed.
    std::vector<std::string> service_ids;
    {
        std::shared_lock lock(impl_->registry_mutex);
        service_ids = impl_->service_order;
    }
    for (const auto& service_id : service_ids) {
        cancel_forked_tasks_for_service(service_id);
        impl_->runtime.timer_manager().cancel_all_for_service(service_id);
        impl_->runtime.coroutine_scheduler().cancel_all_for_service(service_id);
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

        // Step 2c: create a CAF actor for this service before on_init so that
        // on_init-time fork/timer registration can immediately route through
        // the actor path. The service VM is published only after on_init
        // succeeds; until then the actor exists purely as an internal handle.
        std::optional<caf::actor> precreated_actor;
        if (impl_->actor_system) {
            auto actor = impl_->actor_system->spawn(
                [impl_ptr = impl_.get(), manager = this, svc = service_name](
                    caf::event_based_actor* /*self*/) -> caf::behavior {
                    return caf::behavior{[impl_ptr, manager,
                                          svc](const std::string& json_msg) {
                        impl_ptr->dispatch_json_message(manager, svc, json_msg);
                    }};
                });
            {
                std::unique_lock lock(impl_->registry_mutex);
                impl_->service_actors.emplace(service_name, actor);
            }
            precreated_actor = std::move(actor);
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
            impl_->mailboxes[service_name] = std::make_shared<Mailbox>(1000);
        }

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
    constexpr size_t kMaxMessageSize = 1024 * 1024;
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

    // Step 2b: prefer the CAF actor path when an actor system is attached.
    // The actor's behavior delivers the message into the target mailbox and
    // wakes the worker. Fallback to direct mailbox push when no actor system.
    if (impl_->actor_system) {
        std::optional<caf::actor> actor_opt;
        {
            std::shared_lock lock(impl_->registry_mutex);
            auto it = impl_->service_actors.find(service_id);
            if (it != impl_->service_actors.end()) {
                actor_opt = it->second;
            }
        }
        if (actor_opt) {
            const auto now = std::chrono::steady_clock::now();
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();
            const std::string sender = current_service_id();
            auto json_msg = Impl::serialize_message(
                sender, std::string(method), args, impl_->current_trace_id(),
                impl_->current_deadline_ms(), 0, now_ms);
            caf::anon_send(*actor_opt, std::move(json_msg));
            return true;
        }
    }

    std::shared_ptr<Mailbox> mailbox;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto mailbox_it = impl_->mailboxes.find(service_id);
        if (mailbox_it != impl_->mailboxes.end()) {
            mailbox = mailbox_it->second;
        } else if (error) {
            // Distinguish between "never existed" and "exited".
            if (impl_->recently_exited.count(service_id) > 0 ||
                impl_->recently_exited.count(std::string(target)) > 0) {
                *error = "service dead: " + std::string(target);
            } else {
                *error = "service not found: " + std::string(target);
            }
        }
    }
    if (!mailbox) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();

    const std::string sender = current_service_id();

    Mailbox::Message msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.priority = Mailbox::Priority::Normal;
    msg.timestamp_ms = now_ms;

    if (!mailbox->push(msg, Mailbox::Backpressure::DropNewest)) {
        if (error) {
            *error = "mailbox full";
        }
        return false;
    }

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

    // Step 2b: prefer the CAF actor path.
    if (impl_->actor_system) {
        std::optional<caf::actor> actor_opt;
        {
            std::shared_lock lock(impl_->registry_mutex);
            auto it = impl_->service_actors.find(service_id);
            if (it != impl_->service_actors.end()) {
                actor_opt = it->second;
            }
        }
        if (actor_opt) {
            const auto now = std::chrono::steady_clock::now();
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();
            auto json_msg = Impl::serialize_message("", std::string(method),
                                                    args, "", 0, 1, now_ms);
            caf::anon_send(*actor_opt, std::move(json_msg));
            return true;
        }
    }

    std::shared_ptr<Mailbox> mailbox;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto mailbox_it = impl_->mailboxes.find(service_id);
        if (mailbox_it != impl_->mailboxes.end()) {
            mailbox = mailbox_it->second;
        }
    }
    if (!mailbox) {
        if (error) *error = "service not found: " + std::string(target);
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();

    Mailbox::Message msg;
    msg.method = std::string(method);
    msg.args = args;
    msg.priority = Mailbox::Priority::High;
    msg.timestamp_ms = now_ms;

    if (!mailbox->push(msg, Mailbox::Backpressure::DropNewest)) {
        if (error) *error = "mailbox full";
        return false;
    }
    return true;
}

bool LuaServiceManager::send_call_request(std::string_view target,
                                          std::string_view method,
                                          const nlohmann::json& args,
                                          uint64_t session,
                                          std::string* error) {
    const std::string service_id = query_service(target);

    // Step 2b: prefer the CAF actor path.
    if (impl_->actor_system) {
        std::optional<caf::actor> actor_opt;
        {
            std::shared_lock lock(impl_->registry_mutex);
            auto it = impl_->service_actors.find(service_id);
            if (it != impl_->service_actors.end()) {
                actor_opt = it->second;
            }
        }
        if (actor_opt) {
            const auto now = std::chrono::steady_clock::now();
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();
            const std::string sender = current_service_id();
            auto json_msg = Impl::serialize_message(
                sender, std::string(method), args, impl_->current_trace_id(),
                impl_->current_deadline_ms(), 0, now_ms, session, sender);
            caf::anon_send(*actor_opt, std::move(json_msg));
            return true;
        }
    }

    std::shared_ptr<Mailbox> mailbox;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto mailbox_it = impl_->mailboxes.find(service_id);
        if (mailbox_it != impl_->mailboxes.end()) {
            mailbox = mailbox_it->second;
        }
    }
    if (!mailbox) {
        if (error) {
            *error = "service not found: " + std::string(target);
        }
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();

    Mailbox::Message msg;
    msg.sender = current_service_id();
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.priority = Mailbox::Priority::Normal;
    msg.timestamp_ms = now_ms;
    msg.call_session = session;
    msg.call_reply_to = current_service_id();
    if (!mailbox->push(msg, Mailbox::Backpressure::DropNewest)) {
        if (error) {
            *error = "mailbox full";
        }
        return false;
    }
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

    // Step 3: if CAF actor system attached and target has actor, route
    // through the actor (blocking wait on CV). This serializes execution
    // through the actor mailbox instead of synchronous reentry.
    if (impl_->actor_system) {
        std::optional<caf::actor> actor_opt;
        {
            std::shared_lock lock(impl_->registry_mutex);
            auto it = impl_->service_actors.find(service_id);
            if (it != impl_->service_actors.end()) {
                actor_opt = it->second;
            }
        }
        if (actor_opt) {
            // Self-call detection: avoid deadlock (actor mailbox would queue
            // but actor is currently executing this handler).
            if (sender == service_id) {
                return CallResult::error(
                    "self-call not supported via CAF path");
            }

            // Create pending sync call.
            const uint64_t session = impl_->next_sync_call_session.fetch_add(1);
            auto pending = std::make_shared<Impl::PendingSyncCall>();
            pending->session = session;
            {
                std::unique_lock lock(impl_->registry_mutex);
                impl_->pending_sync_calls[session] = pending;
            }

            // Build JSON message for sync_call kind.
            const auto now = std::chrono::steady_clock::now();
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count();
            nlohmann::json j;
            j["kind"] = "sync_call";
            j["sync_session"] = session;
            j["sender"] = sender;
            j["method"] = std::string(method);
            j["args"] = args;
            j["trace_id"] = impl_->current_trace_id();
            j["deadline_ms"] = impl_->current_deadline_ms();
            j["priority"] = 0;
            j["timestamp_ms"] = now_ms;

            // Send to target actor.
            caf::anon_send(*actor_opt, std::move(j.dump()));

            // Block until handler completes (or timeout).
            const int32_t effective_timeout =
                timeout_ms > 0 ? timeout_ms : 5000;
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
    }

    // Fallback: no CAF actor attached — direct synchronous call
    // (original behavior for backward compatibility).
    Impl::DispatchScope scope(*impl_, service_id, sender, false);

    nlohmann::json values = nlohmann::json::array();
    std::string error;
    if (!impl_->runtime.call_service_method(service, method, args, &values,
                                            &error)) {
        return CallResult::error(error);
    }

    std::string exit_service_id;
    std::string exit_reason;
    if (impl_->is_exit_requested(&exit_service_id, &exit_reason) &&
        exit_service_id == service_id) {
        exit(exit_service_id, exit_reason);
    }

    return CallResult::ok(std::move(values));
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
    impl_->runtime.timer_manager().cancel_all_for_service(id);
    impl_->runtime.coroutine_scheduler().cancel_all_for_service(id);
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
    // service. The timeout path (dispatch_json_message / "call_timeout") will
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
        impl_->mailboxes.erase(id);
        impl_->recently_exited.insert(id);

        // Step 2a: tear down the service's CAF actor, if any. anon_send_exit
        // asks the actor to stop; erasing the handle releases our reference.
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

bool LuaServiceManager::process_mailbox(std::string_view service_id) {
    const std::string id(service_id);
    std::shared_ptr<Mailbox> mailbox;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto mailbox_it = impl_->mailboxes.find(id);
        if (mailbox_it != impl_->mailboxes.end()) {
            mailbox = mailbox_it->second;
        }
    }
    if (!mailbox) {
        return false;
    }

    Mailbox::Message msg;
    if (!mailbox->pop(&msg)) {
        return false;  // No messages to process
    }

    return impl_->dispatch_message(this, id, msg);
}

int LuaServiceManager::process_all_mailboxes() {
    int processed = 0;
    // Take a snapshot because process_mailbox may call exit(), which erases
    // from impl_->services and invalidates iterators.
    std::vector<std::string> snapshot;
    {
        std::shared_lock lock(impl_->registry_mutex);
        snapshot = impl_->service_order;
    }
    for (const auto& service_id : snapshot) {
        bool exists = false;
        {
            std::shared_lock lock(impl_->registry_mutex);
            exists = impl_->services.contains(service_id);
        }
        if (!exists) {
            continue;
        }
        if (process_mailbox(service_id)) {
            ++processed;
        }
    }
    return processed;
}

int LuaServiceManager::pump_once() {
    int events = 0;
    events += process_all_mailboxes();

    // Drain forked tasks scheduled by shield.fork. Copy them out first so the
    // task body can itself enqueue new tasks without invalidating iteration.
    std::vector<Impl::ForkedTask> tasks_to_run;
    {
        std::lock_guard<std::mutex> lock(impl_->worker_mutex);
        tasks_to_run.swap(impl_->pending_tasks);
    }
    for (const auto& task : tasks_to_run) {
        Impl::DispatchScope scope(*impl_, task.service_id, "", false);
        if (task.raw_fn.valid()) {
            // Run in a protected call so thrown Lua errors route to on_error.
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
                invoke_error_hook(task.service_id, "fork", "", err);
            }
        } else {
            try {
                task.fn();
            } catch (const std::exception& e) {
                auto& log = shield::log::get_logger("lua");
                SHIELD_LOG_ERROR(log, "forked task " + std::to_string(task.id) +
                                          " error: " + e.what());
                invoke_error_hook(task.service_id, "fork", "", e.what());
            }
        }
        {
            std::lock_guard<std::mutex> lock(impl_->worker_mutex);
            auto it = impl_->tasks_by_service.find(task.service_id);
            if (it != impl_->tasks_by_service.end()) {
                it->second.erase(task.id);
                if (it->second.empty()) {
                    impl_->tasks_by_service.erase(it);
                }
            }
        }
    }
    events += static_cast<int>(tasks_to_run.size());

    const int64_t now = Impl::now_ms();
    // Fire timer callbacks inside a protected call so thrown errors route
    // to the service's on_error hook instead of crashing the pump.
    events += impl_->runtime.timer_manager().check_and_fire_each(
        now, [this](const std::string& service_id, sol::function cb) {
            if (!cb.valid()) return;
            lua_State* L = cb.lua_state();
            cb.push(L);
            int status = lua_pcall(L, 0, 0, 0);
            if (status != LUA_OK) {
                std::string err = "timer error";
                if (lua_type(L, -1) == LUA_TSTRING) {
                    err = lua_tostring(L, -1);
                }
                lua_settop(L, 0);
                invoke_error_hook(service_id, "timer", "", err);
            }
        });
    // Step 2c: when a CAF actor system is attached, both coroutine- and
    // call-timeouts are driven by CAF delayed events rather than a global
    // pump_once scan. Keep the legacy scan only as a no-CAF fallback.
    if (!impl_->uses_caf_actor_system()) {
        events += impl_->runtime.coroutine_scheduler().check_timeouts(now);
        events += check_call_timeouts(now);
    }
    return events;
}

void LuaServiceManager::start_worker() {
    if (impl_->worker_running.load()) {
        return;
    }
    impl_->worker_stop_requested.store(false);
    impl_->worker_thread = std::thread([this]() {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_INFO(log, "Lua worker thread started");
        while (!impl_->worker_stop_requested.load()) {
            try {
                (void)pump_once();
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR(log,
                                 std::string("worker pump error: ") + e.what());
            }
            std::unique_lock<std::mutex> lock(impl_->worker_mutex);
            impl_->worker_cv.wait_for(
                lock, std::chrono::milliseconds(10), [this]() {
                    return impl_->worker_stop_requested.load() ||
                           !impl_->pending_tasks.empty();
                });
        }
        SHIELD_LOG_INFO(log, "Lua worker thread stopped");
    });
    impl_->worker_running.store(true);
}

void LuaServiceManager::stop_worker() {
    if (!impl_->worker_running.load()) {
        return;
    }
    impl_->worker_stop_requested.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->worker_mutex);
        impl_->pending_tasks.clear();
    }
    impl_->worker_cv.notify_all();
    if (impl_->worker_thread.joinable()) {
        impl_->worker_thread.join();
    }
    impl_->worker_running.store(false);
}

uint64_t LuaServiceManager::enqueue_forked_task(std::string service_id,
                                                std::function<void()> task) {
    return enqueue_forked_task(std::move(service_id), std::move(task),
                               sol::function{});
}

uint64_t LuaServiceManager::enqueue_forked_task(std::string service_id,
                                                std::function<void()> task,
                                                sol::function raw_fn) {
    const uint64_t id = impl_->next_task_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(impl_->worker_mutex);
        impl_->pending_tasks.push_back(
            {id, service_id, std::move(task), std::move(raw_fn)});
        impl_->tasks_by_service[service_id].insert(id);
    }

    // Fork tasks execute raw Lua and are often scheduled from inside on_init.
    // Routing them through the CAF service actor would execute the callback on
    // a CAF scheduler thread that may race with the spawning thread still
    // running on_init on the same Lua VM. Keep fork on the pump path (drained
    // by pump_once / the worker), which preserves single-thread execution for
    // the VM. Timers/sleep/timeouts are safe on CAF because their delay
    // guarantees they fire after on_init has returned.
    impl_->worker_cv.notify_one();
    return id;
}

void LuaServiceManager::cancel_forked_tasks_for_service(
    const std::string& service_id) {
    std::unordered_set<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(impl_->worker_mutex);
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
    std::lock_guard<std::mutex> lock(impl_->worker_mutex);
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

    // Step 2c: when a CAF actor system is attached, drive the call timeout via
    // a CAF delayed event instead of relying on pump_once polling.
    if (impl_->uses_caf_actor_system() && !service.empty()) {
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
    if (impl_->uses_caf_actor_system()) {
        cancel_actor_call_timeout(session);
    }

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
    if (count >= Impl::kDefaultMaxErrorsBeforePanic) {
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

void LuaServiceManager::attach_actor_system(caf::actor_system& system) {
    std::unique_lock lock(impl_->registry_mutex);
    impl_->actor_system = &system;
}

bool LuaServiceManager::has_service_actor(const std::string& service_id) const {
    std::shared_lock lock(impl_->registry_mutex);
    return impl_->service_actors.find(service_id) !=
           impl_->service_actors.end();
}

bool LuaServiceManager::uses_caf_actor_system() const {
    std::shared_lock lock(impl_->registry_mutex);
    return impl_->uses_caf_actor_system();
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
        if (!impl_->actor_system) {
            return 0;
        }
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
    auto driver = impl_->actor_system->spawn(
        [svc = service_id, tid = id, target = *service_actor,
         delay_ms](caf::event_based_actor* self) -> caf::behavior {
            self->delayed_send(self, std::chrono::milliseconds(delay_ms),
                               caf::tick_atom_v);
            return caf::behavior{[=](caf::tick_atom) {
                nlohmann::json j;
                j["kind"] = "timer";
                j["timer_id"] = tid;
                caf::anon_send(target, j.dump());
                self->quit();
            }};
        });
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers.end()) {
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
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
        if (!impl_->actor_system) {
            return 0;
        }
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
    auto driver = impl_->actor_system->spawn(
        [svc = service_id, tid = id, target = *service_actor,
         delay_ms](caf::event_based_actor* self) -> caf::behavior {
            self->delayed_send(self, std::chrono::milliseconds(delay_ms),
                               caf::tick_atom_v);
            return caf::behavior{[=](caf::tick_atom) {
                nlohmann::json j;
                j["kind"] = "timer";
                j["timer_id"] = tid;
                caf::anon_send(target, j.dump());
                self->quit();
            }};
        });
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers.end()) {
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
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
        if (!impl_->actor_system) {
            return 0;
        }
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
    auto driver = impl_->actor_system->spawn(
        [svc = service_id, tid = id, target = *service_actor,
         interval_ms](caf::event_based_actor* self) -> caf::behavior {
            self->delayed_send(self, std::chrono::milliseconds(interval_ms),
                               caf::tick_atom_v);
            return caf::behavior{[=](caf::tick_atom) {
                nlohmann::json j;
                j["kind"] = "timer";
                j["timer_id"] = tid;
                caf::anon_send(target, j.dump());
                self->delayed_send(self, std::chrono::milliseconds(interval_ms),
                                   caf::tick_atom_v);
            }};
        });
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_timers.find(id);
        if (it != impl_->actor_timers.end()) {
            it->second.driver = std::move(driver);
            impl_->actor_timers_by_service[service_id].insert(id);
        }
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
        if (!impl_->actor_system) {
            return session;
        }
        auto it = impl_->service_actors.find(service_id);
        if (it == impl_->service_actors.end()) {
            return session;
        }
        service_actor = std::make_shared<caf::actor>(it->second);
    }

    auto driver = impl_->actor_system->spawn(
        [manager = this, session, target = *service_actor,
         timeout_ms](caf::event_based_actor* self) -> caf::behavior {
            self->delayed_send(self, std::chrono::milliseconds(timeout_ms),
                               caf::tick_atom_v);
            return caf::behavior{[=](caf::tick_atom) {
                nlohmann::json j;
                j["kind"] = "call_timeout";
                j["session"] = session;
                caf::anon_send(target, j.dump());
                self->quit();
            }};
        });
    std::unique_lock lock(impl_->registry_mutex);
    impl_->actor_call_timeouts[session] = std::move(driver);
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
