// [SHIELD_LUA] Internal cross-TU declarations for the lua_api_*.cpp split.
//
// Not a public contract: include/shield/lua/lua_api.hpp stays the public
// API. This header only wires the shared helpers and the per-domain
// registration entry points between the lua_api translation units.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "shield/lua/lua_api.hpp"

namespace shield::lua {

// Defined in lua_api.cpp; shared by the domain translation units.
shd::table make_error(shd::this_state state, std::string code,
                      std::string message, bool retryable = false,
                      shd::object detail = shd::nil);
nlohmann::json lua_table_to_json(const shd::table& table);

// Per-domain registration entry points (each defined in its own TU; the
// compiled-out stub variants live behind the same SHIELD_ENABLE_* guards
// as their definitions).
void register_player_api(shd::table& shield, LuaServiceManager* manager);
void register_player_stub_api(shd::table& shield, shd::state_view lua);
void register_server_api(shd::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime);
void register_server_stub_api(shd::table& shield, shd::state_view lua);
void register_global_api(shd::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime);
void register_global_stub_api(shd::table& shield, shd::state_view lua);
void register_http_api(shd::table& shield, LuaServiceManager* manager,
                       LuaRuntime* runtime);
void register_plugin_api(shd::table& shield);

}  // namespace shield::lua
