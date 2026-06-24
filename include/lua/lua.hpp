#pragma once

// sol2 probes <lua/lua.hpp> before <lua.hpp>. Keep this shim in the project
// include path so that probe resolves to the vcpkg Lua headers instead of a
// system /usr/local/include/lua installation with an incompatible Lua version.
#include <lua.hpp>
