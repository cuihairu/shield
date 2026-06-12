// [SHIELD_LUA] Lua Runtime implementation
#include "shield/lua/lua_runtime.hpp"

#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_service.hpp"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>

namespace shield::lua {

// ============================================================================
// CoroutineScheduler Implementation
// ============================================================================

struct CoroutineScheduler::Impl {
    std::unordered_map<CoroutineId, SuspendedCoroutine> suspended;
    std::unordered_map<std::string, std::vector<CoroutineId>> by_service;
    CoroutineId next_id{1};
    std::mutex mutex;

    CoroutineId generate_id() {
        return next_id.fetch_add(1);
    }

    void insert(const SuspendedCoroutine& sc) {
        std::lock_guard<std::mutex> lock(mutex);
        suspended[sc.id] = sc;
        by_service[sc.service_id].push_back(sc.id);
    }

    bool find(CoroutineId id, SuspendedCoroutine* out) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = suspended.find(id);
        if (it != suspended.end()) {
            if (out) *out = it->second;
            return true;
        }
        return false;
    }

    bool erase(CoroutineId id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = suspended.find(id);
        if (it == suspended.end()) {
            return false;
        }

        const std::string service_id = it->second.service_id;
        suspended.erase(it);

        // Remove from by_service index
        auto service_it = by_service.find(service_id);
        if (service_it != by_service.end()) {
            auto& ids = service_it->second;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            if (ids.empty()) {
                by_service.erase(service_it);
            }
        }

        return true;
    }

    std::vector<CoroutineId> get_for_service(const std::string& service_id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_service.find(service_id);
        if (it == by_service.end()) {
            return {};
        }
        return it->second;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return suspended.size();
    }
};

CoroutineScheduler::CoroutineScheduler()
    : impl_(std::make_unique<Impl>()) {}

CoroutineScheduler::CoroutineId CoroutineScheduler::suspend(
    const std::string& service_id,
    sol::coroutine co,
    int32_t timeout_ms) {

    const CoroutineId id = impl_->generate_id();

    // Calculate deadline
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    const int64_t deadline_ms = now_ms + timeout_ms;

    SuspendedCoroutine sc;
    sc.id = id;
    sc.service_id = service_id;
    sc.coroutine = co;
    sc.state = co.lua_state();
    sc.deadline_ms = deadline_ms;
    sc.status = Status::Pending;

    impl_->insert(sc);

    return id;
}

bool CoroutineScheduler::resume(CoroutineId id, const nlohmann::json& result) {
    SuspendedCoroutine sc;
    if (!impl_->find(id, &sc)) {
        return false;  // Already completed or not found
    }

    if (sc.status != Status::Pending) {
        return false;  // Not in pending state
    }

    // Remove from suspended list before resuming
    impl_->erase(id);

    // Set result and resume
    sc.status = Status::Ready;
    sc.result = result;

    // Resume the coroutine with the result
    // We'll convert the JSON array back to Lua values
    auto result_fn = [&](sol::variadic_args args) {
        // Return the result values
        if (result.is_array()) {
            for (const auto& value : result) {
                args.push_back(json_to_lua(sc.state, value));
            }
        }
    };

    // Resume the coroutine
    auto resume_result = sc.coroutine(result_fn);
    if (!resume_result.valid()) {
        sol::error err = resume_result;
        // Log error but still return true as we attempted resume
    }

    return true;
}

bool CoroutineScheduler::resume_with_error(CoroutineId id, const std::string& error) {
    SuspendedCoroutine sc;
    if (!impl_->find(id, &sc)) {
        return false;
    }

    if (sc.status != Status::Pending) {
        return false;
    }

    impl_->erase(id);

    sc.status = Status::Failed;
    sc.error = error;

    // Resume with false, error
    auto resume_result = sc.coroutine(false, error);
    if (!resume_result.valid()) {
        sol::error err = resume_result;
    }

    return true;
}

bool CoroutineScheduler::cancel(CoroutineId id) {
    return impl_->erase(id);
}

void CoroutineScheduler::cancel_all_for_service(const std::string& service_id) {
    auto ids = impl_->get_for_service(service_id);
    for (auto id : ids) {
        cancel(id);
    }
}

int CoroutineScheduler::check_timeouts(int64_t now_ms) {
    int timed_out = 0;
    std::vector<CoroutineId> to_timeout;

    // Collect timed out coroutines
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& [id, sc] : impl_->suspended) {
            if (sc.status == Status::Pending && sc.deadline_ms < now_ms) {
                to_timeout.push_back(id);
            }
        }
    }

    // Resume them with timeout error
    for (auto id : to_timeout) {
        if (resume_with_error(id, "timeout")) {
            ++timed_out;
        }
    }

    return timed_out;
}

size_t CoroutineScheduler::active_count() const {
    return impl_->size();
}

// ============================================================================
// TimerManager Implementation
// ============================================================================

struct TimerManager::Impl {
    struct Timer {
        TimerId id;
        TimerType type;
        int64_t deadline_ms;
        int64_t interval_ms;
        TimerCallback callback;
        bool active = true;
    };

    std::unordered_map<TimerId, Timer> timers;
    std::unordered_map<std::string, std::vector<TimerId>> by_service;
    TimerId next_id{1};
    std::mutex mutex;

    TimerId generate_id() {
        return next_id.fetch_add(1);
    }

    void insert(const Timer& timer) {
        std::lock_guard<std::mutex> lock(mutex);
        timers[timer.id] = timer;
        by_service[timer.callback.service_id].push_back(timer.id);
    }

    bool find(TimerId id, Timer* out) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = timers.find(id);
        if (it != timers.end()) {
            if (out) *out = it->second;
            return true;
        }
        return false;
    }

    bool erase(TimerId id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = timers.find(id);
        if (it == timers.end()) {
            return false;
        }

        const std::string service_id = it->second.callback.service_id;
        timers.erase(it);

        // Remove from by_service index
        auto service_it = by_service.find(service_id);
        if (service_it != by_service.end()) {
            auto& ids = service_it->second;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            if (ids.empty()) {
                by_service.erase(service_it);
            }
        }

        return true;
    }

    std::vector<TimerId> get_for_service(const std::string& service_id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = by_service.find(service_id);
        if (it == by_service.end()) {
            return {};
        }
        return it->second;
    }

    std::vector<Timer> get_expired(int64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Timer> expired;
        for (auto& [id, timer] : timers) {
            if (timer.active && timer.deadline_ms <= now_ms) {
                expired.push_back(timer);
            }
        }
        return expired;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return timers.size();
    }
};

TimerManager::TimerManager()
    : impl_(std::make_unique<Impl>()) {}

TimerManager::TimerId TimerManager::schedule_once(
    int64_t delay_ms,
    sol::function callback,
    const std::string& service_id) {

    const TimerId id = impl_->generate_id();

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    TimerManager::Impl::Timer timer;
    timer.id = id;
    timer.type = TimerType::Once;
    timer.deadline_ms = now_ms + delay_ms;
    timer.interval_ms = delay_ms;
    timer.callback = {callback, sol::state_view(callback.lua_state()), service_id};

    impl_->insert(timer);

    return id;
}

TimerManager::TimerId TimerManager::schedule_fixed_delay(
    int64_t interval_ms,
    sol::function callback,
    const std::string& service_id) {

    const TimerId id = impl_->generate_id();

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    TimerManager::Impl::Timer timer;
    timer.id = id;
    timer.type = TimerType::FixedDelay;
    timer.deadline_ms = now_ms + interval_ms;
    timer.interval_ms = interval_ms;
    timer.callback = {callback, sol::state_view(callback.lua_state()), service_id};

    impl_->insert(timer);

    return id;
}

bool TimerManager::cancel(TimerId id) {
    return impl_->erase(id);
}

void TimerManager::cancel_all_for_service(const std::string& service_id) {
    auto ids = impl_->get_for_service(service_id);
    for (auto id : ids) {
        cancel(id);
    }
}

int TimerManager::check_and_fire(int64_t now_ms) {
    int fired = 0;

    // Get expired timers
    auto expired = impl_->get_expired(now_ms);

    // Fire them
    for (const auto& timer : expired) {
        if (!timer.active) continue;

        // Mark as inactive to prevent double-firing
        impl_->erase(timer.id);

        // Fire the callback
        try {
            timer.callback.callback();
        } catch (const std::exception& e) {
            // Log error
        }

        ++fired;

        // If it's a repeating timer, reschedule
        if (timer.type == TimerType::FixedDelay) {
            const TimerId new_id = schedule_fixed_delay(
                timer.interval_ms,
                timer.callback.callback,
                timer.callback.service_id);
            (void)new_id;
        }
    }

    return fired;
}

size_t TimerManager::active_count() const {
    return impl_->size();
}

// ============================================================================
// Mailbox Implementation
// ============================================================================

struct Mailbox::Impl {
    std::deque<Message> queues[4];  // One queue per priority (Urgent=0, High=1, Normal=2, Low=3)
    size_t max_size;
    std::atomic<size_t> dropped_count{0};
    mutable std::mutex mutex;

    explicit Impl(size_t max_sz) : max_size(max_sz) {}

    size_t total_size() const {
        size_t total = 0;
        for (const auto& q : queues) {
            total += q.size();
        }
        return total;
    }

    bool is_full() const {
        return total_size() >= max_size;
    }

    void push_oldest(const Message& msg) {
        queues[static_cast<int>(msg.priority)].push_back(msg);
    }

    void push_newest(const Message& msg) {
        queues[static_cast<int>(msg.priority)].push_front(msg);
    }

    bool pop_next(Message* out) {
        // Check queues in priority order (Urgent -> High -> Normal -> Low)
        for (auto& q : queues) {
            if (!q.empty()) {
                *out = std::move(q.front());
                q.pop_front();
                return true;
            }
        }
        return false;
    }

    bool drop_oldest() {
        for (int i = 3; i >= 0; --i) {  // Start from lowest priority
            if (!queues[i].empty()) {
                queues[i].pop_back();
                return true;
            }
        }
        return false;
    }
};

Mailbox::Mailbox(size_t max_size)
    : impl_(std::make_unique<Impl>(max_size)) {}

bool Mailbox::push(const Message& msg, Backpressure strategy) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (!impl_->is_full()) {
        impl_->push_oldest(msg);
        return true;
    }

    // Handle backpressure
    switch (strategy) {
        case Backpressure::DropNewest:
            impl_->droled_count.fetch_add(1);
            return false;

        case Backpressure::DropOldest:
            impl_->drop_oldest();
            impl_->push_oldest(msg);
            return true;

        case Backpressure::Block:
            // For Phase 1, we still drop; full blocking requires coroutine support
            impl_->dropped_count.fetch_add(1);
            return false;
    }

    return false;
}

bool Mailbox::pop(Message* out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->pop_next(out);
}

size_t Mailbox::size() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total_size();
}

bool Mailbox::full() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->is_full();
}

void Mailbox::clear() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& q : impl_->queues) {
        q.clear();
    }
}

size_t Mailbox::dropped_count() const {
    return impl_->dropped_count.load();
}

// ============================================================================
// ServiceHandle Implementation
// ============================================================================

void ServiceHandle::register_usertype(sol::state& lua) {
    lua.new_usertype<ServiceHandle>("ServiceHandle",
        // Prevent direct construction from Lua
        "new", sol::no_constructor,

        // Methods
        "id", &ServiceHandle::id,
        "valid", &ServiceHandle::valid,

        // Metamethods
        sol::meta_function::to_string, [](const ServiceHandle& h) {
            return "<ServiceHandle: " + h.id() + ">";
        },

        // Equality by service ID
        sol::meta_function::equal_to, [](const ServiceHandle& a,
                                          const ServiceHandle& b) {
            return a.id() == b.id();
        }
    );
}

// ============================================================================
// LuaVM Implementation
// ============================================================================

// Opaque Lua VM handle
class LuaVM {
public:
    LuaVM() : state_(std::make_shared<sol::state>()) {
        state_->open_libraries(sol::lib::base, sol::lib::string,
                               sol::lib::table, sol::lib::math,
                               sol::lib::io, sol::lib::os);
    }

    std::shared_ptr<sol::state> state() { return state_; }
    sol::table& service_table() { return service_table_; }
    void service_table(sol::table table) { service_table_ = std::move(table); }

private:
    std::shared_ptr<sol::state> state_;
    sol::table service_table_ = sol::nil;
};

struct LuaRuntime::Impl {
    std::shared_ptr<sol::state> default_state;
    LuaServiceManager* service_manager = nullptr;

    Impl() : default_state(std::make_shared<sol::state>()) {
        default_state->open_libraries(sol::lib::base, sol::lib::string,
                                     sol::lib::table, sol::lib::math);
    }
};

LuaRuntime::LuaRuntime()
    : impl_(std::make_unique<Impl>()),
      coroutine_scheduler_(std::make_unique<CoroutineScheduler>()),
      timer_manager_(std::make_unique<TimerManager>()) {}

LuaRuntime::~LuaRuntime() = default;

std::shared_ptr<LuaVM> LuaRuntime::create_vm() {
    return std::make_shared<LuaVM>();
}

bool LuaRuntime::load_script(std::shared_ptr<LuaVM> vm,
                            std::string_view script_path) {
    try {
        auto result = vm->state()->script_file(std::string(script_path));
        return result.valid();
    } catch (const std::exception& e) {
        return false;
    }
}

namespace {

sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value) {
    if (value.is_null()) {
        return sol::make_object(lua, sol::nil);
    }
    if (value.is_boolean()) {
        return sol::make_object(lua, value.get<bool>());
    }
    if (value.is_number_integer()) {
        return sol::make_object(lua, value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return sol::make_object(lua, value.get<std::uint64_t>());
    }
    if (value.is_number_float()) {
        return sol::make_object(lua, value.get<double>());
    }
    if (value.is_string()) {
        return sol::make_object(lua, value.get<std::string>());
    }
    if (value.is_array()) {
        sol::table table = lua.create_table();
        int index = 1;
        for (const auto& item : value) {
            table[index++] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    if (value.is_object()) {
        sol::table table = lua.create_table();
        for (const auto& [key, item] : value.items()) {
            table[key] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    return sol::make_object(lua, sol::nil);
}

bool lua_to_json(const sol::object& value, nlohmann::json* out) {
    if (!out) {
        return true;
    }

    if (!value.valid() || value == sol::nil) {
        *out = nullptr;
        return true;
    }
    if (value.is<bool>()) {
        *out = value.as<bool>();
        return true;
    }
    if (value.is<std::int64_t>()) {
        *out = value.as<std::int64_t>();
        return true;
    }
    if (value.is<int>()) {
        *out = value.as<int>();
        return true;
    }
    if (value.is<double>()) {
        *out = value.as<double>();
        return true;
    }
    if (value.is<std::string>()) {
        *out = value.as<std::string>();
        return true;
    }
    if (!value.is<sol::table>()) {
        *out = "<unsupported>";
        return false;
    }

    sol::table table = value.as<sol::table>();
    bool array_like = true;
    std::size_t max_index = 0;
    std::size_t entry_count = 0;
    for (const auto& [key, _] : table) {
        ++entry_count;
        sol::object key_obj = key;
        if (!key_obj.is<int>()) {
            array_like = false;
            break;
        }
        const int index = key_obj.as<int>();
        if (index <= 0) {
            array_like = false;
            break;
        }
        max_index = std::max(max_index, static_cast<std::size_t>(index));
    }

    if (array_like && max_index == entry_count) {
        nlohmann::json array = nlohmann::json::array();
        for (std::size_t i = 1; i <= max_index; ++i) {
            nlohmann::json item;
            if (!lua_to_json(table[static_cast<int>(i)], &item)) {
                return false;
            }
            array.push_back(std::move(item));
        }
        *out = std::move(array);
        return true;
    }

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, val] : table) {
        sol::object key_obj = key;
        std::string object_key;
        if (key_obj.is<std::string>()) {
            object_key = key_obj.as<std::string>();
        } else if (key_obj.is<int>()) {
            object_key = std::to_string(key_obj.as<int>());
        } else {
            continue;
        }

        nlohmann::json item;
        if (!lua_to_json(val, &item)) {
            return false;
        }
        object[object_key] = std::move(item);
    }
    *out = std::move(object);
    return true;
}

}  // namespace

bool LuaRuntime::load_service_module(std::shared_ptr<LuaVM> vm,
                                     std::string_view script_path,
                                     std::string* error) {
    try {
        sol::state& lua = *vm->state();
        sol::load_result loaded = lua.load_file(std::string(script_path));
        if (!loaded.valid()) {
            sol::error err = loaded;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        sol::protected_function chunk = loaded;
        sol::protected_function_result result = chunk();
        if (!result.valid()) {
            sol::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        sol::object module = result;
        if (!module.is<sol::table>()) {
            if (error) {
                *error = "service module must return a table";
            }
            return false;
        }

        vm->service_table(module.as<sol::table>());
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool LuaRuntime::call_service_function(std::shared_ptr<LuaVM> vm,
                                       std::string_view func_name,
                                       const nlohmann::json& args,
                                       std::string* error) {
    try {
        sol::table& service = vm->service_table();
        if (!service.valid()) {
            if (error) {
                *error = "service module not loaded";
            }
            return false;
        }

        sol::object value = service[std::string(func_name)];
        if (!value.valid() || value == sol::nil) {
            return true;
        }
        if (!value.is<sol::protected_function>()) {
            if (error) {
                *error = std::string(func_name) + " is not a function";
            }
            return false;
        }

        sol::state_view lua(*vm->state());
        sol::protected_function func = value.as<sol::protected_function>();
        sol::protected_function_result result = func(json_to_lua(lua, args));
        if (!result.valid()) {
            sol::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        sol::object first = result.get<sol::object>(0);
        if (first.is<bool>() && !first.as<bool>()) {
            sol::object second = result.get<sol::object>(1);
            if (error) {
                *error = second.valid() && second != sol::nil
                             ? second.as<std::string>()
                             : std::string(func_name) + " returned false";
            }
            return false;
        }
        if (first == sol::nil && result.return_count() > 1) {
            sol::object second = result.get<sol::object>(1);
            if (error) {
                *error = second.valid() && second != sol::nil
                             ? second.as<std::string>()
                             : std::string(func_name) + " returned nil";
            }
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool LuaRuntime::call_service_method(std::shared_ptr<LuaVM> vm,
                                     std::string_view method_name,
                                     const nlohmann::json& args,
                                     nlohmann::json* returns,
                                     std::string* error) {
    try {
        sol::table& service = vm->service_table();
        if (!service.valid()) {
            if (error) {
                *error = "service module not loaded";
            }
            return false;
        }

        sol::object value = service[std::string(method_name)];
        if (!value.valid() || value == sol::nil) {
            if (error) {
                *error = "method not found: " + std::string(method_name);
            }
            return false;
        }
        if (!value.is<sol::protected_function>()) {
            if (error) {
                *error = std::string(method_name) + " is not a function";
            }
            return false;
        }

        if (!args.is_array()) {
            if (error) {
                *error = "method args must be a JSON array";
            }
            return false;
        }

        sol::state_view lua(*vm->state());
        std::vector<sol::object> lua_args;
        lua_args.reserve(args.size());
        for (const auto& arg : args) {
            lua_args.push_back(json_to_lua(lua, arg));
        }

        sol::protected_function func = value.as<sol::protected_function>();
        sol::protected_function_result result = func(sol::as_args(lua_args));
        if (!result.valid()) {
            sol::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        if (returns) {
            *returns = nlohmann::json::array();
            for (int i = 0; i < result.return_count(); ++i) {
                nlohmann::json item;
                if (!lua_to_json(result.get<sol::object>(i), &item)) {
                    if (error) {
                        *error = "unsupported return value from " +
                                 std::string(method_name);
                    }
                    return false;
                }
                returns->push_back(std::move(item));
            }
        }

        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

std::string LuaRuntime::call_function(std::shared_ptr<LuaVM> vm,
                                     std::string_view func_name,
                                     std::string_view args) {
    try {
        sol::protected_function func = (*vm->state())[std::string(func_name)];
        if (!func.valid()) {
            return R"({"error": "function not found"})";
        }

        auto result = func();
        if (result.valid()) {
            return R"({"ok": true})";
        } else {
            sol::error err = result;
            return R"({"error": ")" + std::string(err.what()) + R"("})";
        }
    } catch (const std::exception& e) {
        return R"({"error": ")" + std::string(e.what()) + R"("})";
    }
}

void LuaRuntime::register_api(std::shared_ptr<LuaVM> vm) {
    // Register all shield.* API functions
    register_full_shield_api(*vm->state(), impl_->service_manager, this);
}

void LuaRuntime::set_service_manager(LuaServiceManager* manager) {
    impl_->service_manager = manager;
}

std::string LuaRuntime::get_global(std::shared_ptr<LuaVM> vm,
                                 std::string_view name) {
    sol::state& lua = *vm->state();
    sol::object value = lua[name];

    if (value.is<std::string>()) {
        return value.as<std::string>();
    } else if (value.is<int>()) {
        return std::to_string(value.as<int>());
    } else if (value.is<double>()) {
        return std::to_string(value.as<double>());
    }
    return "";
}

void LuaRuntime::set_global(std::shared_ptr<LuaVM> vm,
                           std::string_view name,
                           std::string_view value) {
    sol::state& lua = *vm->state();
    lua[name] = std::string(value);
}

// ============================================================================
// LuaPack Encoder Implementation
// ============================================================================

LuaPackEncoder::LuaPackEncoder(const Config& config)
    : config_(config) {}

bool LuaPackEncoder::encode(sol::state_view lua, const sol::object& value,
                            std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    error_.clear();

    // Write header: magic (2 bytes) + version (1 byte) + flags (1 byte)
    out_bytes.push_back(MAGIC_HIGH);
    out_bytes.push_back(MAGIC_LOW);
    out_bytes.push_back(VERSION);
    out_bytes.push_back(0);  // flags

    return encode_value(lua, value, out_bytes, 0);
}

bool LuaPackEncoder::encode_value(sol::state_view lua, const sol::object& value,
                                   std::vector<uint8_t>& out, size_t depth) {
    if (depth > config_.max_nesting_depth) {
        error_ = "max nesting depth exceeded";
        return false;
    }

    if (!value.valid() || value == sol::nil) {
        out.push_back(static_cast<uint8_t>(TypeTag::Nil));
        return true;
    }

    if (value.is<bool>()) {
        out.push_back(value.as<bool>() ?
                     static_cast<uint8_t>(TypeTag::True) :
                     static_cast<uint8_t>(TypeTag::False));
        return true;
    }

    if (value.is<int>() || value.is<std::int64_t>()) {
        out.push_back(static_cast<uint8_t>(TypeTag::Integer));
        int64_t val = value.is<int>() ? value.as<int>() : value.as<std::int64_t>();
        // Write int64 in little-endian
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>(val & 0xFF));
            val >>= 8;
        }
        return true;
    }

    if (value.is<double>()) {
        out.push_back(static_cast<uint8_t>(TypeTag::Number));
        double val = value.as<double>();
        // Write double in little-endian
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&val);
        out.insert(out.end(), bytes, bytes + sizeof(double));
        return true;
    }

    if (value.is<std::string>()) {
        std::string str = value.as<std::string>();
        if (str.size() >= 256) {
            if (str.size() > config_.max_string_length) {
                error_ = "string too long";
                return false;
            }
            // Long string
            out.push_back(static_cast<uint8_t>(TypeTag::String));
            uint32_t len = static_cast<uint32_t>(str.size());
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<uint8_t>(len & 0xFF));
                len >>= 8;
            }
        } else {
            // Short string
            out.push_back(static_cast<uint8_t>(TypeTag::ShortString));
            out.push_back(static_cast<uint8_t>(str.size()));
        }
        out.insert(out.end(), str.begin(), str.end());
        return true;
    }

    if (value.is<sol::table>()) {
        sol::table table = value.as<sol::table>();

        // Check if it's an array-like table
        bool array_like = true;
        size_t max_index = 0;
        size_t entry_count = 0;

        for (const auto& [key, _] : table) {
            ++entry_count;
            sol::object key_obj = key;
            if (!key_obj.is<int>()) {
                array_like = false;
                break;
            }
            int index = key_obj.as<int>();
            if (index <= 0) {
                array_like = false;
                break;
            }
            max_index = std::max(max_index, static_cast<size_t>(index));
        }

        if (array_like && max_index == entry_count && max_index <= config_.max_array_length) {
            // Encode as array
            out.push_back(static_cast<uint8_t>(TypeTag::Array));
            uint32_t count = static_cast<uint32_t>(max_index);
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<uint8_t>(count & 0xFF));
                count >>= 8;
            }
            // Encode elements
            for (size_t i = 1; i <= max_index; ++i) {
                if (!encode_value(lua, table[static_cast<int>(i)], out, depth + 1)) {
                    return false;
                }
            }
            return true;
        }

        // Encode as map
        if (entry_count > config_.max_map_entries) {
            error_ = "map has too many entries";
            return false;
        }

        out.push_back(static_cast<uint8_t>(TypeTag::Map));
        uint32_t count = static_cast<uint32_t>(entry_count);
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>(count & 0xFF));
            count >>= 8;
        }

        for (const auto& [key, val] : table) {
            sol::object key_obj = key;
            if (key_obj.is<std::string>() || key_obj.is<int>()) {
                if (!encode_value(lua, key_obj, out, depth + 1)) {
                    return false;
                }
                if (!encode_value(lua, val, out, depth + 1)) {
                    return false;
                }
            } else {
                error_ = "map keys must be string or integer";
                return false;
            }
        }
        return true;
    }

    if (value.is<ServiceHandle>()) {
        out.push_back(static_cast<uint8_t>(TypeTag::ServiceHandle));
        const auto& handle = value.as<ServiceHandle>();
        std::string id = handle.id();
        // For Phase 1, encode service ID as string (deferred: proper node+id encoding)
        return encode_value(lua, sol::make_object(lua, id), out, depth + 1);
    }

    error_ = "unsupported type for LuaPack encoding";
    return false;
}

// ============================================================================
// LuaPack Decoder Implementation
// ============================================================================

LuaPackDecoder::LuaPackDecoder()
    : error_("") {}

sol::object LuaPackDecoder::decode(sol::state_view lua, const std::vector<uint8_t>& bytes,
                                   size_t& out_bytes_consumed) {
    out_bytes_consumed = 0;
    error_.clear();

    if (bytes.size() < 4) {
        error_ = "invalid LuaPack header: too short";
        return sol::make_object(lua, sol::nil);
    }

    // Check magic
    if (bytes[0] != LuaPackEncoder::MAGIC_HIGH ||
        bytes[1] != LuaPackEncoder::MAGIC_LOW) {
        error_ = "invalid LuaPack magic bytes";
        return sol::make_object(lua, sol::nil);
    }

    // Check version
    if (bytes[2] != LuaPackEncoder::VERSION) {
        error_ = "unsupported LuaPack version";
        return sol::make_object(lua, sol::nil);
    }

    out_bytes_consumed = 4;  // Skip header
    return decode_value(lua, bytes.data() + 4, bytes.size() - 4, out_bytes_consumed);
}

sol::object LuaPackDecoder::decode_value(sol::state_view lua, const uint8_t* data,
                                         size_t size, size_t& out_consumed) {
    out_consumed = 0;

    if (size == 0) {
        error_ = "unexpected end of data";
        return sol::make_object(lua, sol::nil);
    }

    uint8_t tag = data[0];
    out_consumed = 1;

    switch (tag) {
        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::Nil):
            return sol::make_object(lua, sol::nil);

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::False):
            return sol::make_object(lua, false);

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::True):
            return sol::make_object(lua, true);

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::Integer):
            if (size < 9) {
                error_ = "truncated integer";
                return sol::make_object(lua, sol::nil);
            }
            {
                int64_t val = 0;
                for (int i = 0; i < 8; ++i) {
                    val |= static_cast<int64_t>(data[1 + i]) << (i * 8);
                }
                out_consumed = 9;
                return sol::make_object(lua, val);
            }

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::Number):
            if (size < 9) {
                error_ = "truncated number";
                return sol::make_object(lua, sol::nil);
            }
            {
                double val;
                std::memcpy(&val, data + 1, sizeof(double));
                out_consumed = 9;
                return sol::make_object(lua, val);
            }

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::ShortString):
            if (size < 2) {
                error_ = "truncated short string";
                return sol::make_object(lua, sol::nil);
            }
            {
                uint8_t len = data[1];
                if (size < 2 + len) {
                    error_ = "truncated short string data";
                    return sol::make_object(lua, sol::nil);
                }
                std::string str(reinterpret_cast<const char*>(data + 2), len);
                out_consumed = 2 + len;
                return sol::make_object(lua, str);
            }

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::String):
            if (size < 5) {
                error_ = "truncated string length";
                return sol::make_object(lua, sol::nil);
            }
            {
                uint32_t len = 0;
                for (int i = 0; i < 4; ++i) {
                    len |= static_cast<uint32_t>(data[1 + i]) << (i * 8);
                }
                if (size < 5 + len) {
                    error_ = "truncated string data";
                    return sol::make_object(lua, sol::nil);
                }
                std::string str(reinterpret_cast<const char*>(data + 5), len);
                out_consumed = 5 + len;
                return sol::make_object(lua, str);
            }

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::Array):
            if (size < 5) {
                error_ = "truncated array length";
                return sol::make_object(lua, sol::nil);
            }
            {
                uint32_t count = 0;
                for (int i = 0; i < 4; ++i) {
                    count |= static_cast<uint32_t>(data[1 + i]) << (i * 8);
                }
                sol::table arr = lua.create_table();
                out_consumed = 5;
                const uint8_t* elem_data = data + 5;
                size_t elem_size = size - 5;
                for (uint32_t i = 0; i < count; ++i) {
                    size_t elem_consumed = 0;
                    sol::object elem = decode_value(lua, elem_data, elem_size, elem_consumed);
                    if (!error_.empty()) {
                        return sol::make_object(lua, sol::nil);
                    }
                    arr[i + 1] = elem;  // Lua arrays are 1-based
                    elem_data += elem_consumed;
                    elem_size -= elem_consumed;
                    out_consumed += elem_consumed;
                }
                return arr;
            }

        case static_cast<uint8_t>(LuaPackEncoder::TypeTag::Map):
            if (size < 5) {
                error_ = "truncated map count";
                return sol::make_object(lua, sol::nil);
            }
            {
                uint32_t count = 0;
                for (int i = 0; i < 4; ++i) {
                    count |= static_cast<uint32_t>(data[1 + i]) << (i * 8);
                }
                sol::table map = lua.create_table();
                out_consumed = 5;
                const uint8_t* entry_data = data + 5;
                size_t entry_size = size - 5;
                for (uint32_t i = 0; i < count; ++i) {
                    size_t key_consumed = 0;
                    sol::object key = decode_value(lua, entry_data, entry_size, key_consumed);
                    if (!error_.empty()) {
                        return sol::make_object(lua, sol::nil);
                    }
                    entry_data += key_consumed;
                    entry_size -= key_consumed;
                    out_consumed += key_consumed;

                    size_t val_consumed = 0;
                    sol::object val = decode_value(lua, entry_data, entry_size, val_consumed);
                    if (!error_.empty()) {
                        return sol::make_object(lua, sol::nil);
                    }
                    entry_data += val_consumed;
                    entry_size -= val_consumed;
                    out_consumed += val_consumed;

                    map[key] = val;
                }
                return map;
            }

        default:
            error_ = "unknown type tag: " + std::to_string(tag);
            return sol::make_object(lua, sol::nil);
    }
}

}  // namespace shield::lua
