#include "shield/serialization/json_serializer.hpp"
#include "shield/core/logger.hpp"

namespace shield::serialization {

std::string JsonSerializer::serialize(const actor::LuaMessage& message) {
    nlohmann::json j;
    j["type"] = message.type;
    j["sender_id"] = message.sender_id;
    j["data"] = message.data;
    return j.dump();
}

actor::LuaMessage JsonSerializer::deserialize_lua_message(const std::string& json_string) {
    try {
        auto j = nlohmann::json::parse(json_string);
        actor::LuaMessage message;
        message.type = j.at("type").get<std::string>();
        message.sender_id = j.at("sender_id").get<std::string>();
        message.data = j.at("data").get<std::unordered_map<std::string, std::string>>();
        return message;
    } catch (const nlohmann::json::exception& e) {
        SHIELD_LOG_ERROR << "JSON deserialization error for LuaMessage: " << e.what();
        return actor::LuaMessage(); // Return empty message on error
    }
}

std::string JsonSerializer::serialize(const actor::LuaResponse& response) {
    nlohmann::json j;
    j["success"] = response.success;
    j["error_message"] = response.error_message;
    j["data"] = response.data;
    return j.dump();
}

actor::LuaResponse JsonSerializer::deserialize_lua_response(const std::string& json_string) {
    try {
        auto j = nlohmann::json::parse(json_string);
        actor::LuaResponse response;
        response.success = j.at("success").get<bool>();
        response.error_message = j.at("error_message").get<std::string>();
        response.data = j.at("data").get<std::unordered_map<std::string, std::string>>();
        return response;
    } catch (const nlohmann::json::exception& e) {
        SHIELD_LOG_ERROR << "JSON deserialization error for LuaResponse: " << e.what();
        return actor::LuaResponse(false, {}, "JSON deserialization error"); // Return error response
    }
}

} // namespace shield::serialization
