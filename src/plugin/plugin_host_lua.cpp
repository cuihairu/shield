// [SHIELD_PLUGIN] PluginHost's Lua integration: package-library
// path injection and the register_lua dispatch, run per Lua VM with
// g_current_lua_state set so host callbacks can reach the active
// state.
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
// Lua integration: path injection + register_lua dispatch.
//
// Both run per Lua VM (services each get their own state). For each VM, the
// host sets g_current_lua_state so the host_api.lua_state / lua_add_path
// callbacks can reach the active state when a plugin queries them during
// register_lua.
// ---------------------------------------------------------------------------
void PluginHost::inject_lua_paths(lua_State* L) {
    if (!L) return;
    lua_State* prev = g_current_lua_state;
    g_current_lua_state = L;
    for (const auto& id : impl_->start_order) {
        const Instance* inst = find_instance(id);
        if (!inst || !inst->package ||      // GCOVR_EXCL_BR_LINE
            inst->state != State::started)  // GCOVR_EXCL_BR_LINE (defensive:
                                            // inject_lua_paths runs on the
                                            // same start_order that start_all
                                            // just consumed, so every id
                                            // resolves to a live instance)
            continue;
        if (!inst->package->manifest.lua.enabled) continue;
        for (const auto& rel : inst->package->manifest.lua.search_paths) {
            if (rel.empty()) continue;
            // Resolve relative to the package root.
            fs::path resolved = inst->package->root / rel;
            std::string s = resolved.string();
#ifdef _WIN32
            std::replace(s.begin(), s.end(), '\\', '/');
#endif
            lua_getglobal(L, "package");
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }
            lua_getfield(L, -1, "path");
            std::string cur = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            if (!cur.empty() && cur.back() != ';') cur.push_back(';');
            cur += s;
            lua_pushlstring(L, cur.c_str(), cur.size());
            lua_setfield(L, -2, "path");
            lua_pop(L, 1);
        }
    }
    g_current_lua_state = prev;
}

bool PluginHost::register_lua_all(lua_State* L, std::string& error) {
    if (!L) return true;
    lua_State* prev = g_current_lua_state;
    g_current_lua_state = L;
    bool ok = true;
    for (const auto& id : impl_->start_order) {
        Instance* inst = find_instance_mut(id);
        if (!inst || !inst->handle ||       // GCOVR_EXCL_BR_LINE
            inst->state != State::started)  // GCOVR_EXCL_BR_LINE (defensive:
                                            // after start_all an instance with
                                            // a non-null handle is always in
                                            // the started state; failed or
                                            // unavailable instances had their
                                            // handle released)
            continue;
        // Transitional: plugins built before register_lua existed have a NULL
        // slot. Treat as "no Lua surface" and skip silently.
        if (!inst->handle->register_lua) continue;
        shield_error_v1 e{};
        if (inst->handle->register_lua(inst->handle, L, &e) != 0) {
            std::string msg = std::string("plugin.lua_register.failed: ") +
                              inst->id +  // GCOVR_EXCL_BR_START
                              (e.code ? " [" + std::string(e.code) + "]" : "") +
                              (e.message ? " " + std::string(e.message) : "");
            // GCOVR_EXCL_BR_STOP (compiler artifact: the filled and null
            // e.code / e.message arms are probed by the register_fail_noerr
            // test; missed arcs are inlined concatenation pseudo-arms)
            if (inst->decl.required) {
                inst->last_error = msg;
                error = msg;
                ok = false;
                break;
            }
            SHIELD_LOG_WARNING(shield::log::get_logger("plugin"),
                               "non-required register_lua failed: " + msg);
            inst->last_error = msg;
        }
    }
    g_current_lua_state = prev;
    return ok;
}

}  // namespace shield::plugin
