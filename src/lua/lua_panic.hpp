// [SHIELD_LUA] Lua panic forensics.
//
// Internal to src/ (deliberately not on the include/shield public face):
// split out of lua_runtime.cpp's anonymous namespace so the coverage
// suites can drive the formatting logic in-process. Defined in
// lua_runtime.cpp; reachable through the shield_lua link target.
#pragma once

#include <cstdio>
#include <string>

struct lua_State;

namespace shield::lua {

// Write the panic forensics block to `sink` (stderr in production) and
// return the one-line error-object description. Emits the "*** shield lua
// panic ctx:" dump and the final "*** shield lua panic:" message line.
// Leaves L's stack exactly as it was found (the production caller aborts
// right after, but the coverage tests run this on live states).
std::string write_lua_panic_forensics(lua_State* L, std::FILE* sink);

}  // namespace shield::lua
