// [SHIELD_LUA] shield.crypto — primitive-level crypto bindings.
//
// Registers the shield.crypto table: base64/base64url/hex codecs, SHA-256,
// HMAC-SHA256, secure random bytes and constant-time comparison. Deliberate
// scope: cryptographic PRIMITIVES only (the "runtime provides primitives,
// business logic composes them" ruling). JWT/session semantics live in the
// Lua layer (scripts/lib/jwt.lua is the reference implementation); auth has
// no place in this table.
#pragma once

#include <sol/forward.hpp>

namespace shield::lua {

/// @brief Register shield.crypto (codec + hash/random primitives) onto the
/// shield table. Called once per VM from register_full_shield_api.
void register_crypto_api(sol::table& shield);

}  // namespace shield::lua
