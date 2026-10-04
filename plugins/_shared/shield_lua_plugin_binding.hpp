// Shared Lua helpers for plugin callable namespaces.
#pragma once

#include <string>

#include "shield/lua/binding.hpp"
#include "shield/plugin/host_api.h"

namespace shield::plugins {

inline shd::table make_module_unavailable_error(shd::state_view lua,
                                                const std::string& binding) {
    shd::table err = lua.create_table();
    err["code"] = "module_unavailable";
    err["message"] = "plugin binding is unavailable";
    err["binding"] = binding;
    return err;
}

inline void push_module_unavailable(shd::variadic_results& results,
                                    shd::state_view lua,
                                    const std::string& binding) {
    results.push_back(shd::make_object(lua, shd::nil));
    results.push_back(
        shd::make_object(lua, make_module_unavailable_error(lua, binding)));
}

// Attach a metatable to a table value (metatable_key assignment parity).
// Raw C API under shd: push both, lua_setmetatable, balanced pop.
inline void set_metatable(shd::table& obj, const shd::table& mt) {
    lua_State* L = obj.lua_state();
    const int i = obj.push();
    mt.push();
    lua_setmetatable(L, i);
    lua_pop(L, 1);  // obj.push()'s copy (lua_setmetatable consumed mt's)
}

// get_or_create<table>() parity for the callable-namespace install
// pattern: read the key, return the existing table, or create + store a
// fresh one. Never clobbers a populated namespace.
inline shd::table get_or_create_global_subtable(shd::state_view lua,
                                                const char* key) {
    shd::object existing = lua.get(key);
    if (existing.is<shd::table>()) return existing.as<shd::table>();
    shd::table t = lua.create_table();
    lua[key] = t;
    return t;
}

// Same for a nested key of an existing table.
inline shd::table get_or_create_subtable(shd::table& parent, const char* key) {
    shd::object existing = parent[key];
    if (existing.is<shd::table>()) return existing.as<shd::table>();
    lua_State* L = parent.lua_state();
    lua_newtable(L);
    shd::table child(L, -1);
    lua_pop(L, 1);
    parent[key] = child;
    return child;
}

template <typename Instance>
Instance* resolve_lua_binding(const shield_host_api_v1* host,
                              shield_plugin_context_v1* ctx,
                              const std::string& binding,
                              Instance* (*find_instance)(const std::string&)) {
    if (!host || !host->binding_instance_id || binding.empty()) {
        return nullptr;
    }
    const char* instance_id = host->binding_instance_id(ctx, binding.c_str());
    if (!instance_id || !instance_id[0]) {
        return nullptr;
    }
    Instance* inst = find_instance(instance_id);
    return inst;
}

}  // namespace shield::plugins
