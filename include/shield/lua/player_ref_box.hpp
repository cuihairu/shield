#pragma once

// PlayerRef userdata box (shield_player P0). The read-only value form a
// __shield_player_ref marker materializes into (see runtime-player.md, the
// PlayerRef contract): it crosses service boundaries as the marker JSON and
// never exposes a mailbox or RPC target.
//
// The whole header is inert unless SHIELD_ENABLE_PLAYER is defined, so
// shield_lua keeps building (with the marker falling through as a plain
// table) when the player module is compiled out. The box holds plain values
// instead of shield::player::PlayerRef for the same reason: no link
// dependency from shield_lua onto shield_player.
#include <cstdint>
#include <string>

#ifdef SHIELD_ENABLE_PLAYER

namespace shield::lua {

struct PlayerRefData {
    std::string uid;
    std::string node_id;
    std::string service_id;
    std::uint64_t epoch = 0;
};

struct PlayerRefBox {
    PlayerRefData data;
};

}  // namespace shield::lua

#endif  // SHIELD_ENABLE_PLAYER
