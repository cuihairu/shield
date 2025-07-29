#include "shield/serialization/json_serializer_new.hpp"
#include "shield/actor/lua_actor.hpp"
#include <nlohmann/json.hpp>
#include <typeinfo>
#include <unordered_map>

namespace shield::serialization {

// Template specializations must be defined before use

// Template specializations for LuaMessage
template<>
std::string JsonSerializer::serialize_impl<actor::LuaMessage>(const actor::LuaMessage& message) {
    nlohmann::json j;
    j["type"] = message.type;
    j["data"] = message.data;
    j["sender_id"] = message.sender_id;
    return j.dump();
}

template<>
void JsonSerializer::deserialize_impl<actor::LuaMessage>(const std::string& json_str, actor::LuaMessage& message) {
    nlohmann::json j = nlohmann::json::parse(json_str);
    message.type = j.value("type", "");
    message.data = j.value("data", std::unordered_map<std::string, std::string>{});
    message.sender_id = j.value("sender_id", "");
}

// Template specializations for LuaResponse
template<>
std::string JsonSerializer::serialize_impl<actor::LuaResponse>(const actor::LuaResponse& response) {
    nlohmann::json j;
    j["success"] = response.success;
    j["data"] = response.data;
    j["error_message"] = response.error_message;
    return j.dump();
}

template<>
void JsonSerializer::deserialize_impl<actor::LuaResponse>(const std::string& json_str, actor::LuaResponse& response) {
    nlohmann::json j = nlohmann::json::parse(json_str);
    response.success = j.value("success", false);
    response.data = j.value("data", std::unordered_map<std::string, std::string>{});
    response.error_message = j.value("error_message", "");
}

// Helper to convert JSON to bytes
std::vector<uint8_t> JsonSerializer::json_to_bytes(const nlohmann::json& j) {
    std::string json_str = j.dump();
    return std::vector<uint8_t>(json_str.begin(), json_str.end());
}

// Helper to convert bytes to JSON
nlohmann::json JsonSerializer::bytes_to_json(const std::vector<uint8_t>& data) {
    std::string json_str(data.begin(), data.end());
    return nlohmann::json::parse(json_str);
}

// Type-erased serialization
std::vector<uint8_t> JsonSerializer::serialize_bytes(const void* object, const std::type_info& type) {
    // Handle known types
    if (type == typeid(actor::LuaMessage)) {
        const auto* msg = static_cast<const actor::LuaMessage*>(object);
        return json_to_bytes(serialize_impl(*msg));
    } else if (type == typeid(actor::LuaResponse)) {
        const auto* resp = static_cast<const actor::LuaResponse*>(object);
        return json_to_bytes(serialize_impl(*resp));
    }
    
    throw std::runtime_error("Unsupported type for JSON serialization: " + std::string(type.name()));
}

void JsonSerializer::deserialize_bytes(const std::vector<uint8_t>& data, void* object, const std::type_info& type) {
    nlohmann::json j = bytes_to_json(data);
    
    // Handle known types
    if (type == typeid(actor::LuaMessage)) {
        auto* msg = static_cast<actor::LuaMessage*>(object);
        deserialize_impl(j.dump(), *msg);
    } else if (type == typeid(actor::LuaResponse)) {
        auto* resp = static_cast<actor::LuaResponse*>(object);
        deserialize_impl(j.dump(), *resp);
    } else {
        throw std::runtime_error("Unsupported type for JSON deserialization: " + std::string(type.name()));
    }
}

// String-based methods optimized for JSON
std::string JsonSerializer::serialize_string(const void* object, const std::type_info& type) {
    // Handle known types directly
    if (type == typeid(actor::LuaMessage)) {
        const auto* msg = static_cast<const actor::LuaMessage*>(object);
        return serialize_impl(*msg);
    } else if (type == typeid(actor::LuaResponse)) {
        const auto* resp = static_cast<const actor::LuaResponse*>(object);
        return serialize_impl(*resp);
    }
    
    throw std::runtime_error("Unsupported type for JSON string serialization: " + std::string(type.name()));
}

void JsonSerializer::deserialize_string(const std::string& data, void* object, const std::type_info& type) {
    // Handle known types
    if (type == typeid(actor::LuaMessage)) {
        auto* msg = static_cast<actor::LuaMessage*>(object);
        deserialize_impl(data, *msg);
    } else if (type == typeid(actor::LuaResponse)) {
        auto* resp = static_cast<actor::LuaResponse*>(object);
        deserialize_impl(data, *resp);
    } else {
        throw std::runtime_error("Unsupported type for JSON string deserialization: " + std::string(type.name()));
    }
}

// Factory function
std::unique_ptr<JsonSerializer> create_json_serializer() {
    return std::make_unique<JsonSerializer>();
}

} // namespace shield::serialization