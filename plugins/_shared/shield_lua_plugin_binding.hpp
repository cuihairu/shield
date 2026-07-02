// Shared Lua helpers for plugin callable namespaces.
#pragma once

#include "shield/plugin/host_api.h"

#include <sol/sol.hpp>

#include <string>

namespace shield::plugins {

inline sol::table make_module_unavailable_error(sol::state_view lua,
                                                const std::string& binding) {
    sol::table err = lua.create_table();
    err["code"] = "module_unavailable";
    err["message"] = "plugin binding is unavailable";
    err["binding"] = binding;
    return err;
}

inline void push_module_unavailable(sol::variadic_results& results,
                                    sol::state_view lua,
                                    const std::string& binding) {
    results.push_back(sol::make_object(lua, sol::nil));
    results.push_back(
        sol::make_object(lua, make_module_unavailable_error(lua, binding)));
}

template <typename Instance>
Instance* resolve_lua_binding(
    const shield_host_api_v1* host,
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
