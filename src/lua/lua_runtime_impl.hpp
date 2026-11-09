// [SHIELD_LUA] Internal cross-TU declarations for the lua_runtime split.
//
// Not a public contract: include/shield/lua/lua_runtime.hpp stays the
// public API. LuaVM and LuaRuntime::Impl live here verbatim so the
// lua_runtime_*.cpp translation units share one definition; the small
// free functions below stay defined in lua_runtime.cpp.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/lua/lua_runtime.hpp"

namespace shield::lua {

// Installs the forensics panic handler on a fresh VM's lua_State.
void install_panic_handler(const std::shared_ptr<shd::state>& state);

// shd object -> JSON conversion shared by the http route handler, the
// call machinery and the module loader.
bool lua_to_json(const shd::object& value, nlohmann::json* out);
nlohmann::json lua_to_json(const shd::object& value);

// ============================================================================
// LuaVM Implementation
// ============================================================================

// Opaque Lua VM handle
// GCOVR_EXCL_START (gcov attribution drift: class header / ctor opening arc
// lands in an uncalled clone on some gcc builds)
class LuaVM {
public:
    LuaVM() : state_(std::make_shared<shd::state>()) {
        // GCOVR_EXCL_STOP
        // Sandbox: lua.sandbox.allow_os / allow_io gate the os and io
        // standard libraries. Unset keys keep the historical behavior
        // (libraries available) so existing deployments that omit the
        // section are not silently changed; the shipped default config
        // declares both false, so a fresh checkout runs sandboxed.
        const bool allow_os =
            shield::config::get_bool("lua.sandbox.allow_os", true);
        const bool allow_io =
            shield::config::get_bool("lua.sandbox.allow_io", true);

        state_->open_libraries(  // GCOVR_EXCL_LINE (line-continuation
            shd::lib::base,      // GCOVR_EXCL_LINE artifact across args)
            shd::lib::package,   // GCOVR_EXCL_LINE (line-continuation artifact)
            shd::lib::string,    // GCOVR_EXCL_LINE (line-continuation artifact)
            shd::lib::table,     // GCOVR_EXCL_LINE (line-continuation artifact)
            shd::lib::math, shd::lib::coroutine);
        if (allow_io) {
            state_->open_libraries(shd::lib::io);
        }
        if (allow_os) {
            state_->open_libraries(shd::lib::os);
        }
        install_panic_handler(state_);

        // Set Lua module search path from configuration
        std::string module_path = shield::config::get(
            "lua.module_path", "scripts/?.lua;scripts/?/init.lua");
        (*state_)["package"]["path"] = module_path;
    }

    std::shared_ptr<shd::state> state() { return state_; }
    shd::table& service_table() { return service_table_; }
    void service_table(shd::table table) { service_table_ = std::move(table); }

private:
    std::shared_ptr<shd::state> state_;
    shd::table service_table_ = shd::nil;
};

// Script cache entry
struct ScriptCacheEntry {
    std::string bytecode;   // Compiled bytecode
    int64_t mtime = 0;      // File modification time
    int64_t last_used = 0;  // Last access time for LRU
};

struct LuaRuntime::Impl {
    std::shared_ptr<shd::state> default_state;
    LuaServiceManager* service_manager = nullptr;

    // Script cache
    LuaCacheConfig cache_config;
    std::unordered_map<std::string, ScriptCacheEntry> script_cache;
    mutable std::mutex cache_mutex;

    // Inbound HTTP routes registered via shield.httpd.*
    std::vector<HttpRouteRegistration> http_routes;
    std::function<void(const std::string&, const std::string&)> http_route_sink;
    mutable std::mutex http_route_mutex;

    // lua_State -> owning VM (for resolving the registering VM during
    // on_init, before the service is published in the manager registry).
    // Entries are lazily pruned when the weak_ptr expires.
    std::unordered_map<lua_State*, std::weak_ptr<LuaVM>> vms_by_state;

    Impl() : default_state(std::make_shared<shd::state>()) {
        default_state->open_libraries(shd::lib::base, shd::lib::string,
                                      shd::lib::table, shd::lib::math);
        install_panic_handler(default_state);

        // Load cache configuration
        cache_config.enabled =
            shield::config::get_bool("lua.cache.enabled", true);
        cache_config.max_size = static_cast<size_t>(
            shield::config::get_int("lua.cache.max_size", 100));
        cache_config.ttl_seconds =
            shield::config::get_int("lua.cache.ttl_seconds", 0);
    }

    // Get current time in milliseconds
    int64_t current_time_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Get file modification time
    int64_t get_file_mtime(const std::string& path) const {
        try {
            auto ftime = std::filesystem::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<
                std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       sctp.time_since_epoch())
                .count();
        } catch (...) {
            return 0;
        }
    }

    // Check if cache entry is valid
    bool is_cache_valid(const ScriptCacheEntry& entry,
                        int64_t current_mtime) const {
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

        // Calculate how many entries to remove (at least 1, or 10% of overage)
        const size_t overage = script_cache.size() - cache_config.max_size;
        size_t to_remove = std::max(size_t(1), overage / 10);

        // Collect all entries with their last_used times for sorting
        std::vector<std::pair<int64_t, std::string>> entries;
        entries.reserve(script_cache.size());
        for (const auto& [path, entry] : script_cache) {
            entries.emplace_back(entry.last_used, path);
        }

        // Sort by last_used time (oldest first)
        std::sort(entries.begin(), entries.end());

        // Remove the oldest entries
        for (size_t i = 0;
             i < to_remove &&     // GCOVR_EXCL_BR_LINE (defensive)
             i < entries.size();  // GCOVR_EXCL_BR_LINE (defensive: the cache
                                  // only grows one entry past max_size)
             ++i) {  // GCOVR_EXCL_BR_LINE (defensive: the cache only grows one
                     // entry past max_size, so to_remove is always 1)
            script_cache.erase(entries[i].second);
        }
    }
};

}  // namespace shield::lua
