// [SHIELD_LUA] Lua API bindings
#pragma once

// This file defines the C++ side of the Lua API
// It registers all shield.* functions into Lua

#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <sol/forward.hpp>

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
sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value);

/// @brief Build a JSON marker object that will become a Lua SessionHandle
/// userdata when passed through json_to_lua().
nlohmann::json make_session_handle_json(
    const std::shared_ptr<shield::net::Session>& session);

/// @brief Full API registration (internal use)
void register_full_shield_api(sol::state& lua,
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
