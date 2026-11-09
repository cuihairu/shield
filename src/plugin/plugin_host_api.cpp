// [SHIELD_PLUGIN] PluginHost's host_api_v1 table: the stateless
// C-ABI callback surface handed to every plugin instance (per-call
// state rides CtxBundle*), the thread-local current-Lua-state slot
// and the Lua service hook storage backing it.
// Split out of plugin_host.cpp.
#include <algorithm>
#include <deque>
#include <lua.hpp>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

#include "plugin_host_impl.hpp"
#include "schema_validator.hpp"
#include "shield/log/logger.hpp"

namespace shield::plugin {

namespace fs = std::filesystem;
// ---------------------------------------------------------------------------
// host_api_v1 table (stateless lambdas; per-instance state via CtxBundle*)
// ---------------------------------------------------------------------------
// Current Lua state for the thread doing inject/register_lua. Used by the
// lua_state host callback so plugins can fetch L from inside create/start
// (though typical use is to take L from the register_lua argument).
thread_local lua_State* g_current_lua_state = nullptr;

namespace {
// Hooks backing lua_current_service_id / lua_post_to_service. Written by
// LuaServiceManager ctor/dtor, read from actor and plugin threads.
std::mutex& lua_hooks_mutex() {
    static std::mutex m;
    return m;
}

LuaServiceHooks& lua_hooks_storage() {
    static LuaServiceHooks hooks;  // GCOVR_EXCL_BR_LINE (static init guard)
    return hooks;
}
}  // namespace

void PluginHost::set_lua_service_hooks(LuaServiceHooks hooks) {
    std::lock_guard lock(lua_hooks_mutex());
    lua_hooks_storage() = std::move(hooks);
}

const shield_host_api_v1& PluginHost::host_api_table() {
    static shield_host_api_v1 api{};
    static bool inited = false;
    if (inited) return api;
    inited = true;

    api.log = [](shield_log_level lv, const char* pkg, const char* inst,
                 const char* msg) {
        auto& log = shield::log::get_logger(pkg ? pkg : "plugin");
        std::string m =  // GCOVR_EXCL_BR_START
            (inst ? std::string("[") + inst + "] " : std::string()) +
            (msg ? msg : "");  // GCOVR_EXCL_BR_STOP
                               // (compiler artifact: the inst/msg null-check
                               // arms are covered by the log battery; the
                               // missed arcs are inlined std::string
                               // concatenation pseudo-branches)
        switch (lv) {          // GCOVR_EXCL_BR_LINE (defensive: all four
                       // shield_log_level enumerators are covered by the log
                       // battery; there is no other value)
            case SHIELD_LOG_DEBUG:
                SHIELD_LOG_DEBUG(log, m);
                break;
            case SHIELD_LOG_INFO:
                SHIELD_LOG_INFO(log, m);
                break;
            case SHIELD_LOG_WARN:
                SHIELD_LOG_WARNING(log, m);
                break;
            case SHIELD_LOG_ERROR:
                SHIELD_LOG_ERROR(log, m);
                break;
        }
    };
    api.report_error = [](const shield_error_v1* err) {
        if (!err) return;
        auto& log = shield::log::get_logger(err->package_id ? err->package_id
                                                            : "plugin");
        SHIELD_LOG_ERROR(  // GCOVR_EXCL_BR_LINE
            log,
            std::string("plugin error [") + (err->code ? err->code : "?") +
                "] " + (err->message ? err->message : "") +
                (err->instance_id ? " instance=" + std::string(err->instance_id)
                                  : "") +
                (err->phase ? " phase=" + std::string(err->phase)
                            : ""));  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // every ternary arm is covered by the
                                     // full/empty shield_error_v1 probes; the
                                     // missed arcs are inlined string
                                     // concatenation pseudo-branches)
    };
    api.config_get = [](shield_plugin_context_v1* ctx,
                        const char* path) -> const char* {
        if (!ctx || !path) return nullptr;
        auto* c = reinterpret_cast<CtxBundle*>(ctx);
        if (!c || !c->instance)  // GCOVR_EXCL_BR_LINE (defensive: ctx is a
                                 // valid CtxBundle handed out at create time,
                                 // and its instance pointer is always set)
            return nullptr;      // GCOVR_EXCL_LINE (same defensive arm)
        // dot-path navigation into the instance's validated config
        const nlohmann::json* cur = &c->instance->decl.config;
        std::string p(path);
        size_t start = 0;
        while (start < p.size() && cur->is_object()) {
            size_t dot = p.find('.', start);
            std::string seg = (dot == std::string::npos)
                                  ? p.substr(start)
                                  : p.substr(start, dot - start);
            if (!cur->contains(seg)) return nullptr;
            cur = &(*cur)[seg];
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        // Contract: valid until next config_get on the same thread.
        thread_local std::string scratch;
        if (cur->is_string())
            scratch = cur->get<std::string>();
        else
            scratch = cur->dump();
        return scratch.c_str();
    };
    api.dependency = [](shield_plugin_context_v1* ctx, const char* name,
                        const char* iface) -> const void* {
        if (!ctx || !name || !iface) return nullptr;
        auto* c = reinterpret_cast<CtxBundle*>(ctx);
        if (!c || !c->host ||  // GCOVR_EXCL_BR_LINE
            !c->instance)  // GCOVR_EXCL_BR_LINE (defensive: the CtxBundle given
                           // to a plugin at create time always has host and
                           // instance set)
            return nullptr;  // GCOVR_EXCL_LINE (same defensive arm)
        auto it = c->instance->decl.dependencies.find(name);
        if (it == c->instance->decl.dependencies.end()) return nullptr;
        if (!c->instance->package)  // GCOVR_EXCL_BR_LINE (defensive: only
                                    // instances attached to a scanned package
                                    // ever get a context)
            return nullptr;         // GCOVR_EXCL_LINE (same defensive arm)
        const auto* req = find_require(c->instance->package->manifest, name);
        if (!req || req->interface_name != iface)  // GCOVR_EXCL_BR_LINE
            return nullptr;  // GCOVR_EXCL_BR_LINE (defensive: resolve
                             // guarantees every configured dependency name is
                             // declared in the manifest, so !req is
                             // unreachable; the interface-mismatch arms are
                             // covered by the dependency probes)
        const Instance* dep = c->host->find_instance(it->second);
        if (!dep || !dep->handle ||        // GCOVR_EXCL_BR_LINE
            dep->state != State::started)  // GCOVR_EXCL_BR_LINE (defensive: a
                                           // declared dependency instance
                                           // always exists, and one with a
                                           // non-null handle is started —
                                           // failed ones had the handle
                                           // released)
            return nullptr;
        shield_error_v1 e{};
        return dep->handle->get_interface(dep->handle, iface, &e);
    };
    api.lua_state = [](shield_plugin_context_v1*) -> lua_State* {
        // Return the Lua state currently being injected/registered on this
        // thread. Outside of inject_lua_paths/register_lua_all this is NULL.
        return g_current_lua_state;
    };
    api.lua_add_path = [](shield_plugin_context_v1* ctx, const char* path,
                          int is_cpath) -> int {
        if (!ctx || !path) return -1;
        auto* c = reinterpret_cast<CtxBundle*>(ctx);
        if (!c || !c->instance ||   // GCOVR_EXCL_BR_LINE
            !c->instance->package)  // GCOVR_EXCL_BR_LINE (defensive: the
                                    // CtxBundle given to a plugin at create
                                    // time always has instance and package
                                    // set)
            return -1;              // GCOVR_EXCL_LINE (same defensive arm)
        lua_State* L = g_current_lua_state;
        if (!L) return -1;

        // Resolve relative paths against the package root.
        fs::path resolved = c->instance->package->root;
        resolved /= path;
        // Convert "lua/?.lua" pattern-style paths to absolute.
        std::string s = resolved.string();
#ifdef _WIN32
        std::replace(s.begin(), s.end(), '\\', '/');
#endif
        lua_getglobal(L, "package");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return -1;
        }
        const char* field = is_cpath ? "cpath" : "path";
        lua_getfield(L, -1, field);
        std::string cur = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!cur.empty() && cur.back() != ';') cur.push_back(';');
        cur += s;
        lua_pushlstring(L, cur.c_str(), cur.size());
        lua_setfield(L, -2, field);
        lua_pop(L, 1);
        return 0;
    };
    api.binding_instance_id = [](shield_plugin_context_v1* ctx,
                                 const char* binding) -> const char* {
        if (!binding || !binding[0]) return nullptr;
        const PluginHost* host = nullptr;
        if (ctx) {
            auto* c = reinterpret_cast<CtxBundle*>(ctx);
            host = c ? c->host   // GCOVR_EXCL_BR_LINE
                     : nullptr;  // GCOVR_EXCL_BR_LINE (defensive: c is ctx
                                 // itself, which was just null-checked)
        }
        if (!host) host = &global_host();

        // Contract: valid until next binding_instance_id on the same thread.
        thread_local std::string scratch;
        scratch = host->binding_instance_id(binding);
        return scratch.empty() ? nullptr : scratch.c_str();
    };
    api.lua_current_service_id = [](shield_plugin_context_v1*) -> const char* {
        LuaServiceHooks hooks;
        {
            std::lock_guard lock(lua_hooks_mutex());
            hooks = lua_hooks_storage();
        }
        if (!hooks.current_service_id) return nullptr;
        // Contract: valid until the next call on the same thread.
        thread_local std::string scratch;
        scratch = hooks.current_service_id();
        return scratch.empty() ? nullptr : scratch.c_str();
    };
    api.lua_post_to_service =
        [](shield_plugin_context_v1*, const char* service_id, void (*fn)(void*),
           void* user_data, void (*destroy_fn)(void*)) -> int {
        if (!service_id || !service_id[0] || !fn) return -1;
        LuaServiceHooks hooks;
        {
            std::lock_guard lock(lua_hooks_mutex());
            hooks = lua_hooks_storage();
        }
        if (!hooks.post_to_service) return -1;

        // Exactly-once destroy semantics: run() marks executed and calls
        // destroy_fn after fn; if the task is dropped without running, the
        // last shared_ptr release calls destroy_fn from the destructor.
        struct PostTask {
            void (*fn)(void*);
            void* user_data;
            void (*destroy_fn)(void*);
            bool executed = false;
            ~PostTask() {
                if (!executed && destroy_fn) destroy_fn(user_data);
            }
            void run() {
                executed = true;
                fn(user_data);
                if (destroy_fn) destroy_fn(user_data);
            }
        };
        auto task = std::make_shared<PostTask>(
            PostTask{fn, user_data, destroy_fn, false});
        const uint64_t id =
            hooks.post_to_service(service_id, [task]() { task->run(); });
        if (id == 0) {
            // Enqueue failed (service gone): the task was never queued, so
            // suppress the destructor's destroy_fn — ownership of user_data
            // stays with the caller per the API contract.
            task->executed = true;
            return -1;
        }
        return 0;
    };
    api.lua_suspend_current = [](shield_plugin_context_v1*, lua_State* L,
                                 int32_t timeout_ms,
                                 const char* tag) -> uint64_t {
        if (!L) return 0;
        LuaServiceHooks hooks;
        {
            std::lock_guard lock(lua_hooks_mutex());
            hooks = lua_hooks_storage();
        }
        if (!hooks.suspend_current) return 0;
        return hooks.suspend_current(L, timeout_ms, tag ? tag : "");
    };
    api.lua_resume_session = [](shield_plugin_context_v1*, uint64_t session,
                                int ok, const char* result_json) -> int {
        if (session == 0) return 1;
        LuaServiceHooks hooks;
        {
            std::lock_guard lock(lua_hooks_mutex());
            hooks = lua_hooks_storage();
        }
        if (!hooks.resume_session) return 1;
        return hooks.resume_session(session, ok != 0,
                                    result_json ? result_json : "")
                   ? 0
                   : 1;
    };
    return api;
}

}  // namespace shield::plugin
