// [SHIELD_LUA] Lua Runtime implementation
#include "shield/lua/lua_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

namespace {
// Mirror of anchor_to_main_thread in lua_service.cpp: re-anchor a sol
// reference onto the main thread's lua_State so a stored lua_State* cannot
// dangle after the registering coroutine is collected by the GC. The
// registry is shared across all threads of one global_State, so re-refering
// the same registry entry from the main thread keeps the pointer alive
// without changing which value the reference names.
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
}  // namespace

// ============================================================================
// ServiceHandle Implementation
// ============================================================================

void ServiceHandle::register_usertype(sol::state& lua) {
    lua.new_usertype<ServiceHandle>(
        "ServiceHandle",
        // Prevent direct construction from Lua
        "new", sol::no_constructor,

        // Methods
        // GCOVR_EXCL_START (wrappers are deduplicated by the linker into
        // the calling TU; behavior tested by ServiceHandleMetamethods)
        "id", &ServiceHandle::id, "node", &ServiceHandle::node, "valid",
        &ServiceHandle::valid,
        // GCOVR_EXCL_STOP

        // GCOVR_EXCL_START (linker dedup artifact)
        // Metamethods
        sol::meta_function::to_string,
        [](const ServiceHandle& h) {
            // GCOVR_EXCL_STOP
            return "<ServiceHandle: " + h.id() +
                   ">";  // GCOVR_EXCL_LINE (linker dedup artifact)
        },

        // GCOVR_EXCL_START (linker dedup artifact)
        // Equality by service ID
        sol::meta_function::equal_to,
        [](const ServiceHandle& a, const ServiceHandle& b) {
            // GCOVR_EXCL_STOP
            return a.id() == b.id();  // GCOVR_EXCL_LINE (linker dedup artifact)
        });
}

// ============================================================================
// LuaVM Implementation
// ============================================================================

// Opaque Lua VM handle
// GCOVR_EXCL_START (gcov attribution drift: class header / ctor opening arc
// lands in an uncalled clone on some gcc builds)
class LuaVM {
public:
    LuaVM() : state_(std::make_shared<sol::state>()) {
        // GCOVR_EXCL_STOP
        state_->open_libraries(
            sol::lib::base, sol::lib::package,
            sol::lib::string,  // GCOVR_EXCL_LINE (line-continuation artifact)
            sol::lib::table,   // GCOVR_EXCL_LINE (line-continuation artifact)
            sol::lib::math, sol::lib::io, sol::lib::os, sol::lib::coroutine);

        // Set Lua module search path from configuration
        std::string module_path = shield::config::get(
            "lua.module_path", "scripts/?.lua;scripts/?/init.lua");
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
    std::string bytecode;   // Compiled bytecode
    int64_t mtime = 0;      // File modification time
    int64_t last_used = 0;  // Last access time for LRU
};

struct LuaRuntime::Impl {
    std::shared_ptr<sol::state> default_state;
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

    Impl() : default_state(std::make_shared<sol::state>()) {
        default_state->open_libraries(sol::lib::base, sol::lib::string,
                                      sol::lib::table, sol::lib::math);

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
        for (size_t i = 0; i < to_remove && i < entries.size(); ++i) {
            script_cache.erase(entries[i].second);
        }
    }
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {}

LuaRuntime::~LuaRuntime() = default;

namespace {

// Segment-wise split of "/a/b" (empty segments skipped).
std::vector<std::string> split_path_segments(const std::string& path) {
    std::vector<std::string> segments;
    size_t pos = 0;
    while (pos < path.size()) {
        const size_t next = path.find('/', pos);
        const std::string seg = path.substr(
            pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!seg.empty()) {
            segments.push_back(seg);
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return segments;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

// Match a ":param"-style pattern against a concrete path; captures params.
bool route_pattern_match(
    const std::string& pattern, const std::string& path,
    std::vector<std::pair<std::string, std::string>>* out_params) {
    const auto pat = split_path_segments(pattern);
    const auto seg = split_path_segments(path);
    if (pat.size() != seg.size()) {
        return false;
    }
    std::vector<std::pair<std::string, std::string>> params;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (!pat[i].empty() && pat[i][0] == ':') {
            params.emplace_back(pat[i].substr(1), seg[i]);
            continue;
        }
        if (pat[i] != seg[i]) {
            return false;
        }
    }
    if (out_params) {
        *out_params = std::move(params);
    }
    return true;
}

}  // namespace

std::shared_ptr<LuaVM> LuaRuntime::vm_for_state(lua_State* L) {
    if (!L) {
        return nullptr;
    }
    // Handlers (and on_init) run as coroutine threads of the service VM, so
    // the calling state is often not the main thread the VM map is keyed by.
    // Resolve the origin main thread via the registry's LUA_RIDX_MAINTHREAD.
    lua_State* origin = L;
    if (lua_pushthread(L) == 0) {
        lua_pop(L, 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
        if (lua_State* main = lua_tothread(L, -1)) {
            origin = main;
        }
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    auto it = impl_->vms_by_state.find(origin);
    if (it == impl_->vms_by_state.end()) {
        return nullptr;
    }
    auto vm = it->second.lock();
    if (!vm) {
        // VM is gone; prune the stale entry.
        impl_->vms_by_state.erase(it);
    }
    return vm;
}

bool LuaRuntime::register_http_route(std::shared_ptr<LuaVM> vm,
                                     const std::string& service_id,
                                     const std::string& method,
                                     const std::string& path,
                                     sol::function handler,
                                     std::string* error) {
    if (service_id.empty()) {
        if (error) {
            *error =
                "shield.httpd requires a running service context "
                "(no current service)";
        }
        return false;
    }
    if (!vm) {
        if (error) {
            *error = "invalid VM";
        }
        return false;
    }
    if (!handler.valid()) {
        if (error) {
            *error = "handler must be a function";
        }
        return false;
    }
    if (path.empty() || path.front() != '/') {
        if (error) {
            *error = "path must start with '/'";
        }
        return false;
    }

    HttpRouteRegistration entry;
    entry.service_id = service_id;
    entry.method = method;
    entry.path = path;
    entry.vm = vm;
    // Routes outlive the registering coroutine: keep the handler's lua_State
    // on the main thread (see anchor_to_main_thread above).
    entry.handler = std::make_shared<sol::function>(
        anchor_to_main_thread(std::move(handler)));

    std::function<void(const std::string&, const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
        // Later registrations replace earlier ones for the same route,
        // mirroring HttpServer::route semantics.
        impl_->http_routes.erase(
            std::remove_if(impl_->http_routes.begin(), impl_->http_routes.end(),
                           [&method, &path](const HttpRouteRegistration& r) {
                               return r.method == method && r.path == path;
                           }),
            impl_->http_routes.end());
        impl_->http_routes.push_back(std::move(entry));
        sink = impl_->http_route_sink;
    }
    if (sink) {
        sink(method, path);
    }
    return true;
}

std::optional<HttpRouteRegistration> LuaRuntime::find_http_route(
    const std::string& method, const std::string& path,
    std::vector<std::pair<std::string, std::string>>* out_params) const {
    std::vector<HttpRouteRegistration> snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
        snapshot = impl_->http_routes;
    }

    std::vector<std::pair<std::string, std::string>> params;
    for (const auto& route : snapshot) {
        if (route.method != method) {
            continue;
        }
        if (route_pattern_match(route.path, path, &params)) {
            if (out_params) {
                *out_params = std::move(params);
            }
            return route;
        }
    }
    return std::nullopt;
}

std::vector<HttpRouteRegistration> LuaRuntime::http_routes() const {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    return impl_->http_routes;
}

size_t LuaRuntime::http_route_count() const {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    return impl_->http_routes.size();
}

void LuaRuntime::remove_http_routes_for_service(const std::string& service_id) {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    impl_->http_routes.erase(
        std::remove_if(impl_->http_routes.begin(), impl_->http_routes.end(),
                       [&service_id](const HttpRouteRegistration& r) {
                           return r.service_id == service_id;
                       }),
        impl_->http_routes.end());
}

void LuaRuntime::set_http_route_sink(
    std::function<void(const std::string&, const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    impl_->http_route_sink = std::move(sink);
}

bool LuaRuntime::call_http_handler(const HttpRouteRegistration& route,
                                   const nlohmann::json& request_json,
                                   nlohmann::json& out_desc,
                                   std::string* error) {
    auto vm = route.vm.lock();
    if (!vm || !vm->state()) {
        if (error) {
            *error = "service VM is gone";
        }
        return false;
    }
    if (!route.handler || !route.handler->valid()) {
        if (error) {
            *error = "handler is not callable";
        }
        return false;
    }

    try {
        sol::state& lua = *vm->state();
        sol::table t = lua.create_table();
        t["method"] = request_json.value("method", "");
        t["path"] = request_json.value("path", "");
        t["query"] = request_json.value("query", "");
        t["body"] = request_json.value("body", "");
        sol::table params_t = lua.create_table();
        if (request_json.contains("params")) {
            for (auto it = request_json["params"].begin();
                 it != request_json["params"].end(); ++it) {
                params_t[it.key()] = it.value().get<std::string>();
            }
        }
        t["params"] = params_t;
        sol::table headers_t = lua.create_table();
        if (request_json.contains("headers")) {
            for (auto it = request_json["headers"].begin();
                 it != request_json["headers"].end(); ++it) {
                headers_t[it.key()] = it.value().get<std::string>();
            }
        }
        t["headers"] = headers_t;

        // Invoke the handler through a checked protected result: with
        // SOL_SAFE_FUNCTION_OBJECTS enabled (the default for debug builds
        // without NDEBUG), converting an errored result straight to
        // sol::object triggers a sol panic and replaces the handler's
        // error text.
        auto handler_result = (*route.handler)(t);
        if (!handler_result.valid()) {
            const sol::error err = handler_result;
            out_desc = nlohmann::json::object();
            out_desc["lua_error"] = std::string(err.what());
            return true;
        }
        sol::object result = handler_result;
        out_desc = nlohmann::json::object();
        out_desc["status"] = 200;
        out_desc["json_body"] = false;

        if (!result.valid() || result == sol::nil) {
            out_desc["status"] = 204;
            out_desc["body"] = "";
            return true;
        }
        if (result.is<std::string>()) {
            out_desc["body"] = result.as<std::string>();
            return true;
        }
        if (!result.is<sol::table>()) {
            if (error) {
                *error = "handler must return a table, string, or nil";
            }
            return false;
        }

        sol::table resp = result.as<sol::table>();
        sol::object status = resp["status"];
        if (status.valid() && status.is<double>()) {
            out_desc["status"] = static_cast<std::int64_t>(status.as<double>());
        }
        sol::object headers = resp["headers"];
        if (headers.valid() && headers.is<sol::table>()) {
            nlohmann::json h = nlohmann::json::object();
            for (const auto& [k, v] : headers.as<sol::table>()) {
                sol::object key = k;
                sol::object value = v;
                if (key.is<std::string>() && value.is<std::string>()) {
                    h[key.as<std::string>()] = value.as<std::string>();
                }
            }
            out_desc["headers"] = std::move(h);
        }
        sol::object body = resp["body"];
        if (body.valid()) {
            if (body.is<std::string>()) {
                out_desc["body"] = body.as<std::string>();
            } else {
                // Non-string body: serialize through the shared Lua->JSON
                // conversion (unsupported values become "<unsupported>").
                nlohmann::json encoded;
                lua_to_json(body, &encoded);
                out_desc["body"] = encoded.dump();
                out_desc["json_body"] = true;
            }
        } else {
            out_desc["body"] = "";
        }
        return true;
    } catch (const sol::error& e) {
        out_desc = nlohmann::json::object();
        out_desc["lua_error"] = std::string(e.what());
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string(e.what());
        }
        return false;
    }
}

std::shared_ptr<LuaVM> LuaRuntime::create_vm() {
    auto vm = std::make_shared<LuaVM>();
    std::lock_guard<std::mutex> lock(impl_->http_route_mutex);
    impl_->vms_by_state[vm->state()->lua_state()] = vm;
    return vm;
}

sol::state& LuaRuntime::vm_state(const std::shared_ptr<LuaVM>& vm) {
    return *vm->state();
}

void LuaRuntime::restrict_vm(std::shared_ptr<LuaVM> vm) {
    if (!vm) return;
    sol::state& state = *vm->state();

    // os: keep time/date/clock (business-clock hooks live there); drop the
    // process- and host-control functions.
    sol::table os_table = state["os"];
    if (os_table.valid()) {
        os_table["execute"] = sol::nil;
        os_table["exit"] = sol::nil;
        os_table["getenv"] = sol::nil;
        os_table["remove"] = sol::nil;
        os_table["rename"] = sol::nil;
        os_table["setlocale"] = sol::nil;
    }
    // io: eval diagnostics need no file or process IO.
    state["io"] = sol::nil;
    // require/package: snippets cannot pull modules off the local disk.
    state["require"] = sol::nil;
    state["package"] = sol::nil;
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
    if (value.is<double>()) {
        // sol2's is<int64_t>() accepts any Lua number when
        // SOL_NUMBER_PRECISION_CHECKS is off (the default), so checking it
        // first would let as<int64_t>() truncate floats (e.g. 3.14 -> 3).
        // Read as double, then only round-trip through int64_t when the
        // value is a whole number so the JSON keeps its original type.
        const double d = value.as<double>();
        const auto as_int = static_cast<std::int64_t>(d);
        if (static_cast<double>(as_int) == d) {
            *out = as_int;
        } else {
            *out = d;
        }
        return true;
    }
    if (value.is<std::string>()) {
        *out = value.as<std::string>();
        return true;
    }
    // Client identity userdata travels through message payloads in its
    // marker form (the inverse of the json_to_lua materialization).
    if (value.is<ClientContextBox>()) {
        *out = value.as<const ClientContextBox&>().data.to_json();
        return true;
    }
    if (value.is<ClientRefBox>()) {
        *out = value.as<const ClientRefBox&>().data.to_json();
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

// Convenience wrapper that returns the JSON value directly.
nlohmann::json lua_to_json(const sol::object& value) {
    nlohmann::json result;
    if (!lua_to_json(value, &result)) {
        return "<unsupported>";
    }
    return result;
}

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

bool LuaRuntime::resolve_service_method(std::shared_ptr<LuaVM> vm,
                                        std::string_view method_name,
                                        sol::function* out,
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
        if (!value.valid() || value == sol::nil ||
            !value.is<sol::protected_function>()) {
            if (error) {
                *error = "method '" + std::string(method_name) +
                         "' is missing or not a function";
            }
            return false;
        }
        if (out != nullptr) {
            *out = value.as<sol::protected_function>();
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

bool LuaRuntime::invoke_coroutine(
    std::shared_ptr<LuaVM> vm, sol::function handler,
    const std::vector<nlohmann::json>& args, const std::string& error_type,
    const std::string& method_label, uint64_t call_session,
    LuaServiceManager* manager, std::string_view service_id, std::string* error,
    bool prepend_ctx, bool degrade_on_factory_failure) {
    // Completion helpers: route the outcome to the pending call request (when
    // present) and to the service error hook on failure.
    auto finish_ok = [&](const nlohmann::json& returns) -> bool {
        if (call_session != 0 && manager != nullptr) {
            manager->complete_call(call_session, true, returns);
        }
        if (manager && !service_id.empty()) {
            manager->reset_error_count(std::string(service_id));
        }
        return true;
    };
    auto finish_err = [&](const std::string& msg) -> bool {
        if (error) *error = msg;
        if (call_session != 0 && manager != nullptr) {
            manager->complete_call(call_session, false,
                                   nlohmann::json::array({msg}));
        }
        if (manager && !service_id.empty()) {
            manager->invoke_error_hook(std::string(service_id), error_type,
                                       method_label, msg);
            // The segment terminated here (error): honor a shield.exit it
            // requested. During spawn-init the spawn path owns the request
            // instead — exit() cannot drive an unpublished service.
            if (!LuaServiceManager::spawn_init_in_progress()) {
                manager->finish_pending_exit(std::string(service_id));
            }
        }
        return false;
    };

    try {
        sol::state_view lua(*vm->state());
        lua_State* L = lua.lua_state();

        // Plain synchronous execution with the same completion routing as the
        // coroutine path. Used when no factory is registered (bare VM) and —
        // for callers that opt in — as the degrade path for a failing
        // factory. The degrade path never passes ctx: it mirrors the
        // historical synchronous dispatch signature.
        auto run_sync = [&](bool with_ctx) -> bool {
            std::vector<sol::object> call_args;
            if (with_ctx) {
                sol::table ctx = lua.create_table();
                call_args.push_back(ctx);
            }
            for (const auto& arg : args) {
                call_args.push_back(json_to_lua(lua, arg));
            }
            sol::protected_function handler_pf = handler;
            sol::protected_function_result fr =
                handler_pf(sol::as_args(call_args));
            if (!fr.valid()) {
                sol::error err = fr;
                std::string msg = err.what();
                // GCOVR_EXCL_START (defensive: sol error messages always
                // carry a "[string ...]:N:" position prefix, so msg is never
                // empty in practice)
                if (msg.empty()) {
                    msg = method_label + " raised an error";
                }
                // GCOVR_EXCL_STOP
                return finish_err(msg);
            }
            nlohmann::json returns = nlohmann::json::array();
            for (int i = 0; i < fr.return_count(); ++i) {
                nlohmann::json item;
                lua_to_json(fr.get<sol::object>(i), &item);
                returns.push_back(std::move(item));
            }
            return finish_ok(returns);
        };

        sol::function factory = lua["__shield_run_handler"];
        if (!factory.valid()) {
            // Bare VM without the shield API: the handler cannot yield, so
            // this degrades to a plain synchronous call with the same
            // completion routing as the coroutine path.
            return run_sync(prepend_ctx);
        }
        if (!handler.valid()) {
            return finish_err("handler is not a function");
        }

        // Dispatch ctx table: sender / trace / deadline from the active
        // dispatch scope (nil when unset). Hooks with a no-ctx signature
        // (on_init receives (args) directly) pass prepend_ctx = false.
        sol::table args_table = lua.create_table();
        int arg_count = 0;
        if (prepend_ctx) {
            sol::table ctx = lua.create_table();
            if (manager != nullptr) {
                std::string sender = manager->current_sender_id();
                ctx["sender"] =
                    sender.empty() ? sol::nil : sol::make_object(lua, sender);
                std::string trace = manager->current_trace_id();
                ctx["trace"] =
                    trace.empty() ? sol::nil : sol::make_object(lua, trace);
                int64_t deadline = manager->current_deadline_ms();
                ctx["deadline"] =
                    deadline <= 0 ? sol::nil : sol::make_object(lua, deadline);
            }
            args_table.add(ctx);
            ++arg_count;
        }
        for (const auto& arg : args) {
            args_table.add(json_to_lua(lua, arg));
            ++arg_count;
        }
        args_table["n"] = arg_count;

        // The factory result stays alive across lua_resume so the thread
        // remains anchored on the main stack; a yielding handler is
        // re-anchored by whichever API suspended it. A factory failure
        // degrades to synchronous dispatch instead of throwing through the
        // runtime.
        sol::protected_function factory_pf = lua["__shield_run_handler"];
        sol::protected_function_result fr = factory_pf(handler, args_table);
        if (!fr.valid()) {
            if (degrade_on_factory_failure) {
                return run_sync(false);
            }
            sol::error err = fr;
            std::string msg = err.what();
            if (msg.empty()) {
                msg = "handler coroutine factory failed";
            }
            return finish_err(msg);
        }
        lua_State* co = lua_tothread(L, -1);
        if (co == nullptr) {
            if (degrade_on_factory_failure) {
                return run_sync(false);
            }
            return finish_err("handler coroutine thread missing");
        }
        // A call request tags its handler coroutine so the completion can be
        // routed back to the caller when the coroutine finishes (possibly
        // much later, after several yields).
        if (call_session != 0 && manager != nullptr) {
            manager->set_handler_call_session(co, call_session);
        }

        int nres = 0;
        const int status = lua_resume(co, L, 0, &nres);
        if (status == LUA_OK) {
            nlohmann::json returns = nlohmann::json::array();
            for (int i = 0; i < nres; ++i) {
                nlohmann::json item;
                sol::stack_object so(sol::state_view(co), i + 1);
                lua_to_json(so, &item);
                returns.push_back(std::move(item));
            }
            // Routes the response to the caller via on_handler_completed when
            // this dispatch serviced a call request.
            if (call_session != 0 && manager != nullptr) {
                manager->on_handler_completed(co, returns);
            }
            if (manager && !service_id.empty()) {
                manager->reset_error_count(std::string(service_id));
                // The segment completed here (LUA_OK): honor a shield.exit
                // it requested. During spawn-init the spawn path owns the
                // request instead — exit() cannot drive an unpublished
                // service.
                if (!LuaServiceManager::spawn_init_in_progress()) {
                    manager->finish_pending_exit(std::string(service_id));
                }
            }
            return true;
        }
        if (status == LUA_YIELD) {
            // Suspended (shield.sleep / call): anchored by the suspending API
            // and resumed by the runtime. Publish the yield so a completion
            // that already arrived on another thread may resume the
            // coroutine (see CallYieldSync in LuaServiceManager).
            if (manager != nullptr) {
                manager->mark_call_yielded(co);
            }
            return true;
        }
        std::string msg = method_label.empty()
                              ? std::string("handler raised an error")
                              : method_label + " raised an error";
        if (lua_type(co, -1) == LUA_TSTRING) {
            msg = lua_tostring(co, -1);
        }
        lua_settop(co, 0);
        if (call_session != 0 && manager != nullptr) {
            manager->on_handler_failed(co, msg);
        }
        if (manager && !service_id.empty()) {
            manager->invoke_error_hook(std::string(service_id), error_type,
                                       method_label, msg);
        }
        if (error) *error = msg;
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LuaRuntime::call_service_method_coroutine(
    std::shared_ptr<LuaVM> vm, std::string_view method_name,
    const nlohmann::json& args, std::string* error, uint64_t call_session,
    LuaServiceManager* manager, std::string_view service_id) {
    auto complete_call_failure = [&](const std::string& msg) {
        if (call_session != 0 && manager != nullptr) {
            manager->complete_call(call_session, false,
                                   nlohmann::json::array({msg}));
        }
    };

    // Synchronous dispatch for VMs without the shield API: the handler
    // cannot yield, so completion happens inline.
    auto fallback_dispatch = [&]() -> bool {
        nlohmann::json returns = nlohmann::json::array();
        std::string fallback_error;
        const bool ok = call_service_method(
            vm, method_name, args, call_session != 0 ? &returns : nullptr,
            &fallback_error);
        if (call_session != 0 && manager != nullptr) {
            if (ok) {
                manager->complete_call(call_session, true, returns);
            } else {
                manager->complete_call(call_session, false,
                                       nlohmann::json::array({fallback_error}));
            }
        }
        if (!ok && error) {
            *error = fallback_error;
        }
        return ok;
    };

    try {
        sol::table& service = vm->service_table();
        if (!service.valid()) {
            const std::string msg = "service module not loaded";
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }
        sol::object value = service[std::string(method_name)];
        if (!value.valid() || value == sol::nil) {
            const std::string msg =
                "method not found: " + std::string(method_name);
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }
        if (!value.is<sol::protected_function>()) {
            const std::string msg =
                std::string(method_name) + " is not a function";
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }
        if (!args.is_array()) {
            const std::string msg = "method args must be a JSON array";
            if (error) *error = msg;
            complete_call_failure(msg);
            return false;
        }

        sol::state_view lua(*vm->state());
        if (!lua["__shield_run_handler"].valid()) {
            // No coroutine factory registered (e.g. a VM without the full
            // shield API). Fall back to a plain synchronous dispatch.
            return fallback_dispatch();
        }

        sol::protected_function handler = value.as<sol::protected_function>();
        std::vector<nlohmann::json> arg_vec(args.begin(), args.end());
        return invoke_coroutine(vm, handler, arg_vec, "handler",
                                std::string(method_name), call_session, manager,
                                service_id, error,
                                /*prepend_ctx=*/true,
                                /*degrade_on_factory_failure=*/true);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        complete_call_failure(e.what());
        return false;
    }
}

bool LuaRuntime::invoke_client_rpc(std::shared_ptr<LuaVM> vm,
                                   sol::function handler,
                                   const ClientIngress& ingress,
                                   std::string* error,
                                   LuaServiceManager* manager,
                                   std::string_view service_id) {
    try {
        sol::state_view lua(*vm->state());
        lua_State* L = lua.lua_state();

        sol::function factory = lua["__shield_run_handler"];
        if (!factory.valid()) {
            // No coroutine factory registered (a VM without the full shield
            // API): client RPC requires coroutine-aware dispatch.
            if (error) *error = "coroutine factory not registered";
            return false;
        }
        if (!handler.valid()) {
            if (error) *error = "handler is not a function";
            return false;
        }
        if (!ingress.decoded_request.has_value()) {
            if (error) *error = "ingress request value missing";
            return false;
        }

        // Build args (ctx, client, request): the dispatch ctx table first,
        // then the client identity (the marker materializes as the read-only
        // ClientContext userdata inside json_to_lua), then the request value.
        sol::table ctx = lua.create_table();
        if (manager != nullptr) {
            std::string sender = manager->current_sender_id();
            ctx["sender"] =
                sender.empty() ? sol::nil : sol::make_object(lua, sender);
            std::string trace = manager->current_trace_id();
            ctx["trace"] =
                trace.empty() ? sol::nil : sol::make_object(lua, trace);
            int64_t deadline = manager->current_deadline_ms();
            ctx["deadline"] =
                deadline <= 0 ? sol::nil : sol::make_object(lua, deadline);
        }

        sol::table args_table = lua.create_table();
        args_table.add(ctx);
        args_table.add(json_to_lua(lua, ingress.context.to_json()));
        args_table.add(json_to_lua(lua, *ingress.decoded_request));
        args_table["n"] = 3;

        sol::protected_function factory_pf = lua["__shield_run_handler"];
        sol::protected_function_result fr = factory_pf(handler, args_table);
        if (!fr.valid()) {
            if (error) *error = "handler coroutine factory failed";
            return false;
        }
        lua_State* co = lua_tothread(L, -1);
        if (co == nullptr) {
            if (error) *error = "handler coroutine thread missing";
            return false;
        }

        int nres = 0;
        const int status = lua_resume(co, L, 0, &nres);
        if (status == LUA_OK || status == LUA_YIELD) {
            if (status == LUA_YIELD && manager != nullptr) {
                // Publish the yield for the yield handshake (CallYieldSync):
                // a completion may already be waiting on another thread.
                manager->mark_call_yielded(co);
            }
            if (manager && !service_id.empty()) {
                manager->reset_error_count(std::string(service_id));
            }
            return true;
        }
        // Error: the error object is on the coroutine's stack. Client ingress
        // is fire-and-forget, so a handler error routes through the service
        // error hook only.
        std::string msg = "client rpc route " +
                          std::to_string(ingress.route_id) + " raised an error";
        if (lua_type(co, -1) == LUA_TSTRING) {
            msg = lua_tostring(co, -1);
        }
        lua_settop(co, 0);
        if (error) *error = msg;
        if (manager && !service_id.empty()) {
            manager->invoke_error_hook(std::string(service_id), "client_rpc",
                                       std::to_string(ingress.route_id), msg);
        }
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LuaRuntime::invoke_hook(std::shared_ptr<LuaVM> vm, const char* hook_name,
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
        SHIELD_LOG_ERROR(
            log, std::string(hook_name) + " hook failed: " + err.what());
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

bool LuaRuntime::register_api(std::shared_ptr<LuaVM> vm, std::string* error) {
    // Register all shield.* API functions
    register_full_shield_api(*vm->state(), impl_->service_manager, this);

    // Inject plugin Lua search paths and dispatch register_lua on every
    // started plugin instance. Each VM gets its own bindings; the host uses
    // a thread-local pointer to expose the active L to plugins that query
    // host_api.lua_state() / lua_add_path() during register_lua.
    lua_State* L = vm->state()->lua_state();
    auto& host = shield::plugin::global_host();
    host.inject_lua_paths(L);
    std::string lua_err;
    if (!host.register_lua_all(L, lua_err)) {
        // GCOVR_EXCL_START (register_lua_all failure at service VM setup
        // needs a native plugin whose register_lua fails; integration
        // context)
        SHIELD_LOG_WARNING(
            shield::log::get_logger("lua"),
            std::string("plugin register_lua failed: ") + lua_err);
        if (error) *error = lua_err;
        return false;
        // GCOVR_EXCL_STOP
    }
    return true;
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
    }
    if (value.is<double>()) {
        const double d = value.as<double>();
        const auto as_int = static_cast<int64_t>(d);
        if (static_cast<double>(as_int) == d) {
            return std::to_string(as_int);
        }
        return std::to_string(d);
    }
    return "";
}

void LuaRuntime::set_global(std::shared_ptr<LuaVM> vm, std::string_view name,
                            std::string_view value) {
    sol::state& lua = *vm->state();
    lua[name] = std::string(value);
}

bool LuaRuntime::exec_lua(std::shared_ptr<LuaVM> vm, const std::string& code,
                          nlohmann::json* result, std::string* error) {
    if (!vm || !vm->state()) {
        if (error) *error = "invalid VM";
        return false;
    }

    sol::state& lua = *vm->state();
    lua_State* L = lua.lua_state();

    // Compile
    int load_status = luaL_loadbuffer(L, code.c_str(), code.size(), "=console");
    if (load_status != LUA_OK) {
        if (error) {
            *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
        }
        lua_pop(L, 1);
        return false;
    }

    // Execute
    int base = lua_gettop(L);  // function is at base
    int call_status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (call_status != LUA_OK) {
        if (error) {
            *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "exec error";
        }
        lua_pop(L, 1);
        return false;
    }

    // Capture return values
    int nresults = lua_gettop(L) - base + 1;
    if (result && nresults > 0) {
        *result = nlohmann::json::array();
        sol::state_view sv(L);
        for (int i = base; i <= lua_gettop(L); ++i) {
            sol::stack_object so(sv, i);
            sol::object obj = so;
            if (obj.is<sol::lua_nil_t>()) {
                result->push_back(nullptr);
            } else if (obj.is<bool>()) {
                result->push_back(obj.as<bool>());
            } else if (obj.is<double>()) {
                // sol2's is<int64_t>() accepts any Lua number when
                // SOL_NUMBER_PRECISION_CHECKS is off (the default), so checking
                // it first would let as<int64_t>() truncate floats (e.g. 2.5 ->
                // 2). Read as double, then only round-trip through int64_t when
                // the value is a whole number so the JSON keeps its original
                // type. Mirrors lua_to_json above.
                const double d = obj.as<double>();
                const auto as_int = static_cast<std::int64_t>(d);
                if (static_cast<double>(as_int) == d) {
                    result->push_back(as_int);
                } else {
                    result->push_back(d);
                }
            } else if (obj.is<std::string>()) {
                result->push_back(obj.as<std::string>());
            } else {
                // For tables, functions, etc. - convert to string via Lua
                // tostring
                lua_getglobal(L, "tostring");
                lua_pushvalue(L, i);
                lua_call(L, 1, 1);
                const char* s = lua_tostring(L, -1);
                result->push_back(s ? std::string(s) : "");
                lua_pop(L, 1);
            }
        }
    }
    // Clean up stack
    lua_settop(L, base - 1);
    return true;
}

// ============================================================================
// LuaPack Encoder Implementation
// ============================================================================

LuaPackEncoder::LuaPackEncoder(const Config& config) : config_(config) {}

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
        out.push_back(value.as<bool>() ? static_cast<uint8_t>(TypeTag::True)
                                       : static_cast<uint8_t>(TypeTag::False));
        return true;
    }

    if (value.is<double>()) {
        const double d = value.as<double>();
        const auto as_int = static_cast<int64_t>(d);
        if (static_cast<double>(as_int) == d) {
            // Whole number: encode as a compact integer.
            out.push_back(static_cast<uint8_t>(TypeTag::Integer));
            int64_t val = as_int;
            // Write int64 in little-endian
            for (int i = 0; i < 8; ++i) {
                out.push_back(static_cast<uint8_t>(val & 0xFF));
                val >>= 8;
            }
            return true;
        }
        out.push_back(static_cast<uint8_t>(TypeTag::Number));
        // Write double in little-endian using explicit bit manipulation
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof(double));
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>(bits & 0xFF));
            bits >>= 8;
        }
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

        if (array_like && max_index == entry_count &&
            max_index <= config_.max_array_length) {
            // Encode as array
            out.push_back(static_cast<uint8_t>(TypeTag::Array));
            uint32_t count = static_cast<uint32_t>(max_index);
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<uint8_t>(count & 0xFF));
                count >>= 8;
            }
            // Encode elements
            for (size_t i = 1; i <= max_index; ++i) {
                if (!encode_value(lua, table[static_cast<int>(i)], out,
                                  depth + 1)) {
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
        // For Phase 1, encode service ID as string (deferred: proper node+id
        // encoding)
        return encode_value(lua, sol::make_object(lua, id), out, depth + 1);
    }

    error_ = "unsupported type for LuaPack encoding";
    return false;
}

// ============================================================================
// LuaPack Decoder Implementation
// ============================================================================

LuaPackDecoder::LuaPackDecoder() : error_("") {}

sol::object LuaPackDecoder::decode(sol::state_view lua,
                                   const std::vector<uint8_t>& bytes,
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
    return decode_value(lua, bytes.data() + 4, bytes.size() - 4,
                        out_bytes_consumed);
}

sol::object LuaPackDecoder::decode_value(sol::state_view lua,
                                         const uint8_t* data, size_t size,
                                         size_t& out_consumed) {
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
                // Read double in little-endian using explicit bit manipulation
                uint64_t bits = 0;
                for (int i = 0; i < 8; ++i) {
                    bits |= static_cast<uint64_t>(data[1 + i]) << (i * 8);
                }
                double val;
                std::memcpy(&val, &bits, sizeof(double));
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
                    sol::object elem =
                        decode_value(lua, elem_data, elem_size, elem_consumed);
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
                    sol::object key =
                        decode_value(lua, entry_data, entry_size, key_consumed);
                    if (!error_.empty()) {
                        return sol::make_object(lua, sol::nil);
                    }
                    entry_data += key_consumed;
                    entry_size -= key_consumed;
                    out_consumed += key_consumed;

                    size_t val_consumed = 0;
                    sol::object val =
                        decode_value(lua, entry_data, entry_size, val_consumed);
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

LuaCacheConfig LuaRuntime::cache_config() const { return impl_->cache_config; }

}  // namespace shield::lua
