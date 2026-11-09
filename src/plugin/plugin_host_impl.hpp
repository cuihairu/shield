// [SHIELD_PLUGIN] Internal cross-TU declarations for the PluginHost
// split.
//
// Not a public contract: the public API stays in
// shield/plugin/plugin_host.hpp. The two file-level symbols shared
// across the split are declared here once: the manifest requirement
// lookup (defined flat in plugin_host.cpp, consumed by the host_api
// resolve callback) and the thread-local current-Lua-state slot
// (defined in plugin_host_api.cpp, set by the Lua integration TU).
#pragma once

#include <string_view>

#include "shield/plugin/plugin_host.hpp"

namespace shield::plugin {

// Defined in plugin_host.cpp: manifest requirement lookup shared by
// plan_and_resolve and the host_api resolve callback.
const Manifest::Require* find_require(const Manifest& manifest,
                                      std::string_view name);

// Defined in plugin_host_api.cpp: the Lua state of the thread running
// inject_lua_paths / register_lua_all; the host_api lua_state callback
// hands it to plugins querying it during create/start/register_lua.
extern thread_local struct lua_State* g_current_lua_state;

}  // namespace shield::plugin
