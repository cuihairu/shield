// [SHIELD_LUA] Lua userdata wrappers for the trusted client identity
#pragma once

#include "shield/core/service_message.hpp"

namespace shield::lua {

// ClientContext materializes whenever a __shield_client_ref marker arrives in
// a message payload; ClientRef is what shield.client.bind returns. Both wrap
// the same identity snapshot, expose read-only properties, and serialize back
// to the marker form inside lua_to_json (which needs the concrete types here,
// not just inside lua_api.cpp where they are registered as usertypes).
struct ClientContextBox {
    ClientContextData data;
};

struct ClientRefBox {
    ClientContextData data;
};

}  // namespace shield::lua
