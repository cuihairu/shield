// [SHIELD_LUA] Lua service implementation
#include "shield/lua/lua_service.hpp"
#include "shield/lua/lua_runtime.hpp"

#include "shield/base/error.hpp"
#include "shield/base/result.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    std::unordered_map<std::string, std::unordered_set<std::string>> owned_names;
    std::unordered_map<std::string, std::string> module_scripts;
    std::unordered_map<std::string, std::shared_ptr<Mailbox>> mailboxes;
    std::vector<std::string> service_order;

    struct DispatchFrame {
        std::string service_id;
        std::string sender_id;
        bool in_exit = false;
        bool exit_requested = false;
        std::string exit_reason = "normal";
    };

    std::vector<DispatchFrame> dispatch_stack;

    // Forked task queue. Drained only by the worker thread (or pump_once on
    // the caller's thread when no worker is running).
    struct ForkedTask {
        uint64_t id;
        std::string service_id;
        std::function<void()> fn;
    };
    std::atomic<uint64_t> next_task_id{1};
    std::vector<ForkedTask> pending_tasks;
    std::unordered_map<std::string, std::unordered_set<uint64_t>> tasks_by_service;

    // Worker thread lifecycle. The worker drives mailboxes, timers, forked
    // tasks, and coroutine timeouts. While the worker is running, all Lua
    // execution happens on the worker thread (modulo shield.call reentry,
    // which stays inline because it's already on the worker).
    std::atomic<bool> worker_running{false};
    std::atomic<bool> worker_stop_requested{false};
    std::thread worker_thread;
    std::mutex worker_mutex;
    std::condition_variable worker_cv;

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
        std::string global_script_path = shield::config::get("lua.script_path", "scripts");
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
        if (dispatch_stack.empty()) {
            return "";
        }
        return dispatch_stack.back().service_id;
    }

    std::string current_sender_id() const {
        if (dispatch_stack.empty()) {
            return "";
        }
        return dispatch_stack.back().sender_id;
    }

    bool is_exit_requested(std::string* service_id, std::string* reason) const {
        if (dispatch_stack.empty() || !dispatch_stack.back().exit_requested) {
            return false;
        }
        if (service_id) {
            *service_id = dispatch_stack.back().service_id;
        }
        if (reason) {
            *reason = dispatch_stack.back().exit_reason;
        }
        return true;
    }

    static bool valid_name(std::string_view name) {
        if (name.empty() || name.size() > 64 || name.rfind("shield.", 0) == 0) {
            return false;
        }
        for (char ch : name) {
            const bool ok = (ch >= 'a' && ch <= 'z') ||
                            (ch >= 'A' && ch <= 'Z') ||
                            (ch >= '0' && ch <= '9') ||
                            ch == '_' || ch == '.' || ch == '-';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    class DispatchScope {
    public:
        DispatchScope(Impl& impl,
                      std::string service_id,
                      std::string sender_id,
                      bool in_exit)
            : impl_(impl) {
            impl_.dispatch_stack.push_back({
                std::move(service_id),
                std::move(sender_id),
                in_exit,
                false,
                "normal",
            });
        }

        ~DispatchScope() {
            impl_.dispatch_stack.pop_back();
        }

        DispatchScope(const DispatchScope&) = delete;
        DispatchScope& operator=(const DispatchScope&) = delete;

    private:
        Impl& impl_;
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
    const auto service_ids = impl_->service_order;
    for (const auto& service_id : service_ids) {
        cancel_forked_tasks_for_service(service_id);
        impl_->runtime.timer_manager().cancel_all_for_service(service_id);
        impl_->runtime.coroutine_scheduler().cancel_all_for_service(service_id);
    }
    impl_->runtime.set_service_manager(nullptr);
}

SpawnResult LuaServiceManager::spawn(std::string_view module,
                                     std::string_view opts_json) {
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
            service_name += std::to_string(std::hash<std::string_view>{}(module));
        }
        if (impl_->services.contains(service_name)) {
            return SpawnResult::error("service already exists: " + service_name);
        }
        if (impl_->published_names.contains(service_name)) {
            return SpawnResult::error("service name already exists: " + service_name);
        }
        if (!Impl::valid_name(service_name)) {
            return SpawnResult::error("invalid service name: " + service_name);
        }

        const std::string script_path = impl_->resolve_module(module);

        // Create VM and load module
        auto vm = impl_->runtime.create_vm();
        impl_->runtime.register_api(vm);

        std::string error;
        if (!impl_->runtime.load_service_module(vm, script_path, &error)) {
            return SpawnResult::error("Failed to load module: " +
                                      script_path + ": " + error);
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
            Impl::DispatchScope scope(*impl_, service_name, "", false);
            if (!impl_->runtime.call_service_function(vm, "on_init", init_args,
                                                      &error)) {
                if (auto names_it = impl_->owned_names.find(service_name);
                    names_it != impl_->owned_names.end()) {
                    for (const auto& name : names_it->second) {
                        impl_->published_names.erase(name);
                    }
                    impl_->owned_names.erase(names_it);
                }
                return SpawnResult::error("on_init failed for " + service_name +
                                          ": " + error);
            }
            std::string exit_service_id;
            exit_after_init =
                impl_->is_exit_requested(&exit_service_id, &exit_reason) &&
                exit_service_id == service_name;
        }

        impl_->services[service_name] = std::move(vm);
        impl_->published_names[service_name] = service_name;
        impl_->owned_names[service_name].insert(service_name);
        impl_->service_order.push_back(service_name);

        // Create mailbox for the service
        impl_->mailboxes[service_name] = std::make_shared<Mailbox>(1000);

        if (exit_after_init) {
            exit(service_name, exit_reason);
        }
        return SpawnResult::ok(service_name);

    } catch (const std::exception& e) {
        return SpawnResult::error(std::string("Spawn failed: ") + e.what());
    }
}

bool LuaServiceManager::send(std::string_view target,
                             std::string_view method,
                             const nlohmann::json& args,
                             std::string* error) {
    const std::string service_id = query_service(target);

    auto mailbox_it = impl_->mailboxes.find(service_id);
    if (mailbox_it == impl_->mailboxes.end()) {
        if (error) {
            *error = "service not found: " + std::string(target);
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    const std::string sender = current_service_id();

    Mailbox::Message msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.priority = Mailbox::Priority::Normal;
    msg.timestamp_ms = now_ms;

    if (!mailbox_it->second->push(msg, Mailbox::Backpressure::DropNewest)) {
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
    (void)timeout_ms;

    const std::string service_id = query_service(target);
    auto service_it = impl_->services.find(service_id);
    if (service_it == impl_->services.end()) {
        return CallResult::error("service not found: " + std::string(target));
    }

    const std::string sender = current_service_id();
    Impl::DispatchScope scope(*impl_, service_id, sender, false);

    nlohmann::json values = nlohmann::json::array();
    std::string error;
    if (!impl_->runtime.call_service_method(service_it->second, method, args,
                                            &values, &error)) {
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
    auto it = impl_->services.find(std::string(service_id));
    if (it == impl_->services.end()) {
        return;
    }

    std::string error;
    nlohmann::json args = std::string(reason);
    Impl::DispatchScope scope(*impl_, std::string(service_id), "", true);
    (void)impl_->runtime.call_service_function(it->second, "on_exit", args, &error);

    // Cancel forked tasks / timers / coroutines BEFORE erasing the service VM.
    // These hold sol::function / std::function callbacks that reference the
    // service's lua_State; releasing them after the VM is destroyed would
    // luaL_unref on a closed state.
    cancel_forked_tasks_for_service(std::string(service_id));
    impl_->runtime.timer_manager().cancel_all_for_service(std::string(service_id));
    impl_->runtime.coroutine_scheduler().cancel_all_for_service(std::string(service_id));

    if (auto names_it = impl_->owned_names.find(std::string(service_id));
        names_it != impl_->owned_names.end()) {
        for (const auto& name : names_it->second) {
            impl_->published_names.erase(name);
        }
        impl_->owned_names.erase(names_it);
    }
    impl_->services.erase(it);
    impl_->service_order.erase(
        std::remove(impl_->service_order.begin(), impl_->service_order.end(),
                    std::string(service_id)),
        impl_->service_order.end());

    // Clean up mailbox
    impl_->mailboxes.erase(std::string(service_id));
}

void LuaServiceManager::shutdown_all(std::string_view reason) {
    std::unordered_set<std::string> seen;
    const auto order = impl_->service_order;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (seen.insert(*it).second && impl_->services.contains(*it)) {
            exit(*it, reason);
        }
    }
    impl_->service_order.clear();
}

std::string LuaServiceManager::current_service_id() const {
    return impl_->current_service_id();
}

std::string LuaServiceManager::current_sender_id() const {
    return impl_->current_sender_id();
}

void LuaServiceManager::request_current_exit(std::string_view reason) {
    if (impl_->dispatch_stack.empty()) {
        return;
    }
    auto& frame = impl_->dispatch_stack.back();
    if (frame.in_exit) {
        return;
    }
    frame.exit_requested = true;
    frame.exit_reason = reason.empty() ? "normal" : std::string(reason);
}

std::string LuaServiceManager::query_service(std::string_view name) const {
    auto it = impl_->published_names.find(std::string(name));
    if (it == impl_->published_names.end()) {
        return "";
    }
    return it->second;
}

bool LuaServiceManager::register_name(std::string_view name, std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "register requires current service context";
        }
        return false;
    }
    const bool in_current_dispatch = !impl_->dispatch_stack.empty() &&
                                     impl_->dispatch_stack.back().service_id ==
                                         owner;
    if (!impl_->services.contains(owner) && !in_current_dispatch) {
        if (error) {
            *error = "current service is not running: " + owner;
        }
        return false;
    }
    if (!Impl::valid_name(name)) {
        if (error) {
            *error = "invalid service name: " + std::string(name);
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

bool LuaServiceManager::unregister_name(std::string_view name, std::string* error) {
    const std::string owner = current_service_id();
    if (owner.empty()) {
        if (error) {
            *error = "unregister requires current service context";
        }
        return false;
    }

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
    services.reserve(impl_->published_names.size());
    for (const auto& [name, _] : impl_->published_names) {
        services.push_back(name);
    }
    std::sort(services.begin(), services.end());
    return services;
}

bool LuaServiceManager::process_mailbox(std::string_view service_id) {
    auto mailbox_it = impl_->mailboxes.find(std::string(service_id));
    if (mailbox_it == impl_->mailboxes.end()) {
        return false;
    }

    Mailbox::Message msg;
    if (!mailbox_it->second->pop(&msg)) {
        return false;  // No messages to process
    }

    auto service_it = impl_->services.find(std::string(service_id));
    if (service_it == impl_->services.end()) {
        return false;  // Service no longer exists
    }

    // Process the message
    Impl::DispatchScope scope(*impl_, std::string(service_id), msg.sender, false);

    nlohmann::json values = nlohmann::json::array();
    std::string error;
    if (!impl_->runtime.call_service_method(service_it->second, msg.method, msg.args,
                                            &values, &error)) {
        // Method failed - log error but continue processing other messages
    }

    std::string exit_service_id;
    std::string exit_reason;
    if (impl_->is_exit_requested(&exit_service_id, &exit_reason) &&
        exit_service_id == service_id) {
        exit(exit_service_id, exit_reason);
    }

    return true;
}

int LuaServiceManager::process_all_mailboxes() {
    int processed = 0;
    // Take a snapshot because process_mailbox may call exit(), which erases
    // from impl_->services and invalidates iterators.
    const auto snapshot = impl_->service_order;
    for (const auto& service_id : snapshot) {
        if (!impl_->services.contains(service_id)) {
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
        try {
            task.fn();
        } catch (const std::exception& e) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_ERROR(log, "forked task " + std::to_string(task.id) +
                                  " error: " + e.what());
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
    events += impl_->runtime.timer_manager().check_and_fire(now);
    events += impl_->runtime.coroutine_scheduler().check_timeouts(now);
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
                SHIELD_LOG_ERROR(log, std::string("worker pump error: ") + e.what());
            }
            std::unique_lock<std::mutex> lock(impl_->worker_mutex);
            impl_->worker_cv.wait_for(lock, std::chrono::milliseconds(10),
                [this]() {
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
    const uint64_t id = impl_->next_task_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(impl_->worker_mutex);
        impl_->pending_tasks.push_back({id, service_id, std::move(task)});
        impl_->tasks_by_service[service_id].insert(id);
    }
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

}  // namespace shield::lua
