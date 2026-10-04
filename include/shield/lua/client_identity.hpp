// [SHIELD_LUA] Lua userdata wrappers for the trusted client identity
#pragma once

#include "shield/core/service_message.hpp"
#include "shield/lua/binding.hpp"

namespace shield::lua {

// ClientContext materializes whenever a __shield_client_ref marker arrives in a
// message payload; ClientRef is what shield.client.bind returns. Both wrap
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

// Values of these types push into Lua as usertype userdata (parity).
template <>
struct shd::is_usertype_value<shield::lua::ClientContextBox> : std::true_type {
};
template <>
struct shd::is_usertype_value<shield::lua::ClientRefBox> : std::true_type {};

// These usertypes are registered in B1 (register_client_identity_api).
// They must stay shd-native (raw T + type-name tag + shd metatable): a foreign
// binding stores usertype payloads as a usertype_storage<T> header, so a
// shd-built bare T* box under a foreign metatable fails the foreign
// extraction. Foreign-created values are read through the B1 adapter
// (box_context_marker), which routes by the layout tag.
template <>
struct shd::is_foreign_usertype<shield::lua::ClientContextBox>
    : std::false_type {};
template <>
struct shd::is_foreign_usertype<shield::lua::ClientRefBox> : std::false_type {};
