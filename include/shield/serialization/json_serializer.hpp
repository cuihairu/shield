#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "shield/actor/lua_actor.hpp"
#include "shield/serialization/serializer.hpp"

namespace shield::serialization {

// Legacy JsonSerializer for backward compatibility
class JsonSerializer {
public:
    // Legacy static methods - kept for backward compatibility
    static std::string serialize(const actor::LuaMessage& message);
    static actor::LuaMessage deserialize_lua_message(const std::string& json_string);
    static std::string serialize(const actor::LuaResponse& response);
    static actor::LuaResponse deserialize_lua_response(const std::string& json_string);
};

} // namespace shield::serialization
