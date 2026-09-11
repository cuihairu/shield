// [SHIELD_CORE] ClientContextData JSON marker serialization.
//
// The marker shape ("__shield_client_ref": true plus the identity fields)
// is the wire form of a trusted client reference inside ordinary service
// message payloads: the Lua API layer recognizes it on the way in
// (materializing a read-only ClientContext userdata) and re-emits it on the
// way out (a userdata passed as a message argument). Keeping the format in
// one place next to the type it serializes.
#include "shield/core/service_message.hpp"

namespace shield::lua {

namespace {

constexpr const char* kMarkerKey = "__shield_client_ref";

}  // namespace

nlohmann::json ClientContextData::to_json() const {
    return nlohmann::json{
        {kMarkerKey, true},
        {"gateway_address", gateway_address},
        {"session_id", session_id},
        {"session_epoch", session_epoch},
        {"player_id", player_id},
        {"protocol_profile_id", protocol_profile_id},
    };
}

std::optional<ClientContextData> ClientContextData::from_json(
    const nlohmann::json& json) {
    if (!json.is_object() || !json.contains(kMarkerKey) ||
        !json[kMarkerKey].is_boolean() || !json[kMarkerKey].get<bool>()) {
        return std::nullopt;
    }
    ClientContextData data;
    if (auto it = json.find("gateway_address");
        it != json.end() && it->is_string()) {
        data.gateway_address = it->get<std::string>();
    }
    if (auto it = json.find("session_id");
        it != json.end() && it->is_number_unsigned()) {
        data.session_id = it->get<uint64_t>();
    }
    if (auto it = json.find("session_epoch");
        it != json.end() && it->is_number_unsigned()) {
        data.session_epoch = it->get<uint32_t>();
    }
    if (auto it = json.find("player_id"); it != json.end() && it->is_string()) {
        data.player_id = it->get<std::string>();
    }
    if (auto it = json.find("protocol_profile_id");
        it != json.end() && it->is_string()) {
        data.protocol_profile_id = it->get<std::string>();
    }
    return data;
}

}  // namespace shield::lua
