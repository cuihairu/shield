// [SHIELD_LUA] Lua API bindings
#pragma once

// This file defines the C++ side of the Lua API
// It registers all shield.* functions into Lua

#include <memory>
#include <nlohmann/json_fwd.hpp>

#include "shield/lua/binding.hpp"

namespace shield::net {
class Session;
}

namespace shield::lua {

class LuaRuntime;
class LuaServiceManager;

/// @brief Register the shield.* API into Lua
/// This is called during Lua VM initialization
void register_shield_api(LuaRuntime& runtime);

/// @brief Convert JSON values into Lua values using Shield's runtime rules.
/// Special transport/runtime marker objects may map to userdata instead of
/// plain tables.
/// Canonical converter on the shd surface.
shd::object json_to_lua(shd::state_view lua, const nlohmann::json& value);

/// @brief Convert Lua values to JSON.
/// This is the primary conversion function with error handling via return
/// value.
/// @param value The Lua object to convert
/// @param out Output parameter for the resulting JSON value (set to nullptr on
/// nil)
/// @return true if conversion succeeded, false if unsupported type
bool lua_to_json(const shd::object& value, nlohmann::json* out);

/// @brief Convenience wrapper for lua_to_json that returns the value directly.
/// Returns nullptr for nil values and "<unsupported>" string for unsupported
/// types. Prefer the output-parameter version for better error handling.
nlohmann::json lua_to_json(const shd::object& value);

/// @brief Marker JSON for a box-wrapped client-identity
/// userdata. The payload travels inside a userdata box, so field access
/// must go through the box accessor; shd only identifies the metatable.
nlohmann::json box_context_marker(const shd::object& value);

/// @brief Marker JSON for a box-wrapped PlayerRefBox.
nlohmann::json box_player_marker(const shd::object& value);

/// @brief Register one shield.client_rpc.<name> helper bound to a
/// server-to-client descriptor route. Called per service VM at spawn time,
/// after the descriptor table is compiled. (Takes the raw state so both
/// binding surfaces can call it.)
void register_client_rpc_helper(lua_State* lua, LuaServiceManager* manager,
                                std::string_view name, uint32_t route_id);

/// @brief Full API registration (internal use)
void register_full_shield_api(lua_State* lua,
                              LuaServiceManager* manager = nullptr,
                              LuaRuntime* runtime = nullptr);

/// @brief API categories organized by domain
namespace api {

/// @brief Register service API (spawn, exit, self, names, query, register)
void register_service_api(LuaRuntime& runtime);

/// @brief Register message API (send, call, sender, trace, deadline)
void register_message_api(LuaRuntime& runtime);

/// @brief Register timer API (timer_once, timer, cancel_timer, sleep)
void register_timer_api(LuaRuntime& runtime);

/// @brief Register task API (fork, cancel_task)
void register_task_api(LuaRuntime& runtime);

/// @brief Register config API (config)
void register_config_api(LuaRuntime& runtime);

/// @brief Register log API (log.debug, log.info, log.warn, log.error)
void register_log_api(LuaRuntime& runtime);

/// @brief Register gateway API (session operations)
void register_gateway_api(LuaRuntime& runtime);

}  // namespace api

}  // namespace shield::lua
