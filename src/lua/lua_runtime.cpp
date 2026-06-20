// [SHIELD_LUA] Lua Runtime implementation
#include "shield/lua/lua_runtime.hpp"

#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace shield::lua {

// ============================================================================
// CoroutineScheduler Implementation
// ============================================================================

struct CoroutineScheduler::Impl {
    std::unordered_map<CoroutineId, SuspendedCoroutine> suspended;
    std::unordered_map<std::string, std::vector<CoroutineId>> by_service;
    std::atomic<CoroutineId> next_id{1};
    mutable std::mutex mutex;

    CoroutineId generate_id() {
        return next_id.fetch_add(1);
    }

    void insert(const SuspendedCoroutine& sc) {
        std::lock_guard<std::mutex> lock(mutex);
        suspended[sc.id] = sc;
        by_service[sc.service_id].push_back(sc.id);
    }

    SuspendedCoroutine* find_ptr(CoroutineId id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = suspended.find(id);
        if (it != suspended.end()) {
            return &it->second;
        }
        return nullptr;
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

    SuspendedCoroutine sc = {
        id,
        service_id,
        co,
        deadline_ms,
        Status::Pending,
        {},
        ""
    };

    impl_->insert(sc);

    return id;
}

bool CoroutineScheduler::resume(CoroutineId id, const nlohmann::json& result) {
    SuspendedCoroutine* sc_ptr = impl_->find_ptr(id);
    if (!sc_ptr) {
        return false;  // Not found
    }

    if (sc_ptr->status != Status::Pending) {
        return false;  // Not in pending state
    }

    // Copy the coroutine before erasing
    sol::coroutine coroutine = sc_ptr->coroutine;

    // Remove from suspended list before resuming
    impl_->erase(id);

    // Resume the coroutine with the result
    // For now, we pass the JSON string as a single value
    // TODO: Convert JSON array to multiple Lua values
    auto resume_result = coroutine(result.dump());
    if (!resume_result.valid()) {
        sol::error err = resume_result;
        // Log error but still return true as we attempted resume
    }

    return true;
}

bool CoroutineScheduler::resume_with_error(CoroutineId id, const std::string& error) {
    SuspendedCoroutine* sc_ptr = impl_->find_ptr(id);
    if (!sc_ptr) {
        return false;  // Not found
    }

    if (sc_ptr->status != Status::Pending) {
        return false;  // Not in pending state
    }

    // Copy the coroutine before erasing
    sol::coroutine coroutine = sc_ptr->coroutine;

    impl_->erase(id);

    // Resume with false, error
    auto resume_result = coroutine(false, error);
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
    std::atomic<TimerId> next_id{1};
    mutable std::mutex mutex;

    TimerId generate_id() {
        return next_id.fetch_add(1);
    }

    void insert(const Timer& timer) {
        std::lock_guard<std::mutex> lock(mutex);
        timers.insert(std::make_pair(timer.id, timer));
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

    return schedule_once_fn(delay_ms,
        [cb = std::move(callback)]() {
            if (cb.valid()) {
                auto r = cb();
                (void)r;
            }
        },
        service_id);
}

TimerManager::TimerId TimerManager::schedule_once_fn(
    int64_t delay_ms,
    std::function<void()> callback,
    const std::string& service_id) {

    const TimerId id = impl_->generate_id();

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    TimerManager::Impl::Timer timer = {
        id,
        TimerType::Once,
        now_ms + delay_ms,
        delay_ms,
        {std::move(callback), service_id},
        true
    };

    impl_->insert(timer);

    return id;
}

TimerManager::TimerId TimerManager::schedule_fixed_delay(
    int64_t interval_ms,
    sol::function callback,
    const std::string& service_id) {

    return schedule_fixed_delay_fn(interval_ms,
        [cb = std::move(callback)]() {
            if (cb.valid()) {
                auto r = cb();
                (void)r;
            }
        },
        service_id);
}

TimerManager::TimerId TimerManager::schedule_fixed_delay_fn(
    int64_t interval_ms,
    std::function<void()> callback,
    const std::string& service_id) {

    const TimerId id = impl_->generate_id();

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    TimerManager::Impl::Timer timer = {
        id,
        TimerType::FixedDelay,
        now_ms + interval_ms,
        interval_ms,
        {std::move(callback), service_id},
        true
    };

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
    return check_and_fire(now_ms, nullptr);
}

int TimerManager::check_and_fire(int64_t now_ms,
                                  std::function<void(const std::string& service_id,
                                                     const std::string& error)> on_error) {
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
            if (on_error) {
                on_error(timer.callback.service_id, e.what());
            }
        }

        ++fired;

        // If it's a repeating timer, reschedule
        if (timer.type == TimerType::FixedDelay) {
            const TimerId new_id = schedule_fixed_delay_fn(
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

Mailbox::~Mailbox() = default;

bool Mailbox::push(const Message& msg, Backpressure strategy) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    if (!impl_->is_full()) {
        impl_->push_oldest(msg);
        return true;
    }

    // Handle backpressure
    switch (strategy) {
        case Backpressure::DropNewest:
            impl_->dropped_count.fetch_add(1);
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
        state_->open_libraries(sol::lib::base, sol::lib::package,
                               sol::lib::string, sol::lib::table,
                               sol::lib::math, sol::lib::io, sol::lib::os,
                               sol::lib::coroutine);

        // Set Lua module search path from configuration
        std::string module_path = shield::config::get("lua.module_path",
            "scripts/?.lua;scripts/?/init.lua");
        (*state_)["package"]["path"] = module_path;
    }

    std::shared_ptr<sol::state> state() { return state_; }
    sol::table& service_table() { return service_table_; }
    void service_table(sol::table table) { service_table_ = std::move(table); }

private:
    std::shared_ptr<sol::state> state_;
    sol::table service_table_ = sol::nil;
};

// Script cache entry
struct ScriptCacheEntry {
    std::string bytecode;      // Compiled bytecode
    int64_t mtime = 0;        // File modification time
    int64_t last_used = 0;    // Last access time for LRU
};

struct LuaRuntime::Impl {
    std::shared_ptr<sol::state> default_state;
    LuaServiceManager* service_manager = nullptr;

    // Script cache
    LuaCacheConfig cache_config;
    std::unordered_map<std::string, ScriptCacheEntry> script_cache;
    mutable std::mutex cache_mutex;

    Impl() : default_state(std::make_shared<sol::state>()) {
        default_state->open_libraries(sol::lib::base, sol::lib::string,
                                     sol::lib::table, sol::lib::math);

        // Load cache configuration
        cache_config.enabled = shield::config::get_bool("lua.cache.enabled", true);
        cache_config.max_size = static_cast<size_t>(
            shield::config::get_int("lua.cache.max_size", 100));
        cache_config.ttl_seconds = shield::config::get_int("lua.cache.ttl_seconds", 0);
    }

    // Get current time in milliseconds
    int64_t current_time_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Get file modification time
    int64_t get_file_mtime(const std::string& path) const {
        try {
            auto ftime = std::filesystem::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now()
                + std::chrono::system_clock::now());
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                sctp.time_since_epoch()).count();
        } catch (...) {
            return 0;
        }
    }

    // Check if cache entry is valid
    bool is_cache_valid(const ScriptCacheEntry& entry, int64_t current_mtime) const {
        // Check if file was modified
        if (entry.mtime != current_mtime) {
            return false;
        }
        // Check TTL
        if (cache_config.ttl_seconds > 0) {
            const int64_t ttl_ms = cache_config.ttl_seconds * 1000;
            if (current_time_ms() - entry.last_used > ttl_ms) {
                return false;
            }
        }
        return true;
    }

    // Evict oldest entries if cache is full
    void evict_if_needed() {
        if (script_cache.size() <= cache_config.max_size) {
            return;
        }

        // Find the oldest entry (LRU)
        auto oldest_it = script_cache.begin();
        int64_t oldest_time = oldest_it->second.last_used;

        for (auto it = script_cache.begin(); it != script_cache.end(); ++it) {
            if (it->second.last_used < oldest_time) {
                oldest_it = it;
                oldest_time = it->second.last_used;
            }
        }

        if (oldest_it != script_cache.end()) {
            script_cache.erase(oldest_it);
        }
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
        const std::string path_str(script_path);

        // Try to load from cache
        std::string source_code;
        bool use_cache = false;

        if (impl_->cache_config.enabled) {
            std::lock_guard<std::mutex> lock(impl_->cache_mutex);

            const int64_t current_mtime = impl_->get_file_mtime(path_str);
            auto it = impl_->script_cache.find(path_str);

            if (it != impl_->script_cache.end() &&
                impl_->is_cache_valid(it->second, current_mtime)) {
                // Cache hit
                source_code = it->second.bytecode;
                it->second.last_used = impl_->current_time_ms();
                use_cache = true;
            }
        }

        // Load from file if cache miss or disabled
        if (!use_cache) {
            // Read file content
            try {
                std::ifstream file(path_str);
                if (!file.is_open()) {
                    if (error) {
                        *error = "Failed to open file: " + path_str;
                    }
                    return false;
                }
                std::stringstream buffer;
                buffer << file.rdbuf();
                source_code = buffer.str();
            } catch (const std::exception& e) {
                if (error) {
                    *error = "Failed to read file: " + std::string(e.what());
                }
                return false;
            }

            // Update cache if enabled
            if (impl_->cache_config.enabled) {
                std::lock_guard<std::mutex> lock(impl_->cache_mutex);

                ScriptCacheEntry entry;
                entry.bytecode = source_code;
                entry.mtime = impl_->get_file_mtime(path_str);
                entry.last_used = impl_->current_time_ms();

                impl_->evict_if_needed();
                impl_->script_cache[path_str] = std::move(entry);
            }
        }

        // Load and execute the source code
        sol::load_result loaded = lua.load(source_code, path_str);
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

bool LuaRuntime::call_service_method_coroutine(std::shared_ptr<LuaVM> vm,
                                               std::string_view method_name,
                                               const nlohmann::json& args,
                                               std::string* error,
                                               uint64_t call_session,
                                               LuaServiceManager* manager,
                                               std::string_view service_id) {
    try {
        sol::table& service = vm->service_table();
        if (!service.valid()) {
            if (error) *error = "service module not loaded";
            return false;
        }
        sol::object value = service[std::string(method_name)];
        if (!value.valid() || value == sol::nil) {
            if (error) *error = "method not found: " + std::string(method_name);
            return false;
        }
        if (!value.is<sol::protected_function>()) {
            if (error) *error = std::string(method_name) + " is not a function";
            return false;
        }
        if (!args.is_array()) {
            if (error) *error = "method args must be a JSON array";
            return false;
        }

        sol::state_view lua(*vm->state());
        lua_State* L = lua.lua_state();

        sol::function factory = lua["__shield_run_handler"];
        if (!factory.valid()) {
            // No coroutine factory registered (e.g. a VM without the full
            // shield API). Fall back to a plain synchronous dispatch.
            nlohmann::json unused;
            return call_service_method(vm, method_name, args, &unused, error);
        }

        sol::protected_function handler = value.as<sol::protected_function>();
        sol::table args_table = lua.create_table();
        for (const auto& arg : args) {
            args_table.add(json_to_lua(lua, arg));
        }

        // Call the factory to build a handler coroutine. Use a protected call
        // so a factory failure degrades to synchronous dispatch instead of
        // throwing through the runtime. The result `fr` is kept alive across
        // lua_resume so the thread stays anchored on the main stack; if the
        // handler yields, shield.sleep has already re-anchored it via its own
        // registry ref before yielding.
        sol::protected_function factory_pf = lua["__shield_run_handler"];
        sol::protected_function_result fr = factory_pf(handler, args_table);
        if (!fr.valid()) {
            sol::error err = fr;
            return call_service_method(vm, method_name, args, nullptr, error);
        }
        // The factory returns the coroutine thread at the top of the stack.
        // Grab its lua_State* via the C API (sol::thread's type check is
        // stricter than necessary here). fr stays alive across lua_resume so
        // the thread remains anchored on the main stack; if the handler
        // yields, shield.sleep has re-anchored it via its own registry ref.
        lua_State* co = lua_tothread(L, -1);
        if (co == nullptr) {
            return call_service_method(vm, method_name, args, nullptr, error);
        }
        // If this dispatch services a coroutine call request, tag the handler
        // coroutine so its completion can route the response back to the caller.
        if (call_session != 0 && manager != nullptr) {
            manager->set_handler_call_session(co, call_session);
        }

        int nres = 0;
        const int status = lua_resume(co, L, 0, &nres);
        if (status == LUA_OK) {
            // Handler completed synchronously. Collect its return values and,
            // if this was a call request, resume the suspended caller.
            if (call_session != 0 && manager != nullptr) {
                nlohmann::json returns = nlohmann::json::array();
                for (int i = 0; i < nres; ++i) {
                    nlohmann::json item;
                    sol::stack_object so(sol::state_view(co), i + 1);
                    lua_to_json(so, &item);
                    returns.push_back(std::move(item));
                }
                manager->on_handler_completed(co, returns);
            }
            if (manager && !service_id.empty()) {
                manager->reset_error_count(std::string(service_id));
            }
            return true;
        }
        if (status == LUA_YIELD) {
            // Handler suspended (e.g. shield.sleep). It is anchored against GC
            // by whatever caused the yield and will be resumed by the runtime.
            return true;
        }
        // Error: the error object is on the coroutine's stack.
        std::string msg = std::string(method_name) + " raised an error";
        if (lua_type(co, -1) == LUA_TSTRING) {
            msg = lua_tostring(co, -1);
        }
        lua_settop(co, 0);
        if (error) *error = msg;
        if (manager && !service_id.empty()) {
            manager->invoke_error_hook(std::string(service_id), "handler",
                                       std::string(method_name), msg);
        }
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LuaRuntime::invoke_hook(std::shared_ptr<LuaVM> vm,
                              const char* hook_name,
                              const std::string& err_or_reason,
                              const std::string& error_type,
                              const std::string& method_name) {
    if (!vm) {
        return false;
    }
    sol::table& service = vm->service_table();
    if (!service.valid()) {
        return false;
    }
    sol::object hook_fn = service[hook_name];
    if (!hook_fn.is<sol::protected_function>()) {
        return false;
    }
    sol::state_view lua(vm->state()->lua_state());
    sol::table ctx = lua.create_table();
    ctx["type"] = error_type;
    ctx["method"] = method_name;
    sol::protected_function fn = hook_fn.as<sol::protected_function>();
    auto result = fn(err_or_reason, ctx);
    if (!result.valid()) {
        sol::error err = result;
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_ERROR(log, std::string(hook_name) + " hook failed: " + err.what());
        return false;
    }
    return true;
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

LuaServiceManager* LuaRuntime::service_manager() const {
    return impl_->service_manager;
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

// LuaRuntime cache management methods
void LuaRuntime::clear_cache() {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex);
    impl_->script_cache.clear();
}

size_t LuaRuntime::cache_size() const {
    std::lock_guard<std::mutex> lock(impl_->cache_mutex);
    return impl_->script_cache.size();
}

LuaCacheConfig LuaRuntime::cache_config() const {
    return impl_->cache_config;
}

}  // namespace shield::lua
