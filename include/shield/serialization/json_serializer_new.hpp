#pragma once

#include "shield/serialization/serializer.hpp"
#include <nlohmann/json.hpp>

namespace shield::serialization {

class JsonSerializer : public ISerializer {
public:
    JsonSerializer() = default;
    ~JsonSerializer() override = default;

    // Type-erased serialization
    std::vector<uint8_t> serialize_bytes(const void* object, const std::type_info& type) override;
    void deserialize_bytes(const std::vector<uint8_t>& data, void* object, const std::type_info& type) override;
    
    // String-based convenience methods optimized for JSON
    std::string serialize_string(const void* object, const std::type_info& type) override;
    void deserialize_string(const std::string& data, void* object, const std::type_info& type) override;
    
    SerializationFormat get_format() const override { return SerializationFormat::JSON; }

    // Template methods for type-safe operations
    template<typename T>
    std::string serialize(const T& object) {
        return serialize_impl(object);
    }
    
    template<typename T>
    T deserialize(const std::string& json_str) {
        T object;
        deserialize_impl(json_str, object);
        return object;
    }

private:
    // Type-specific serialization implementations
    template<typename T>
    std::string serialize_impl(const T& object);
    
    template<typename T>
    void deserialize_impl(const std::string& json_str, T& object);
    
    // Helper to convert JSON to/from bytes
    std::vector<uint8_t> json_to_bytes(const nlohmann::json& j);
    nlohmann::json bytes_to_json(const std::vector<uint8_t>& data);
};

// Factory function to create and register JSON serializer
std::unique_ptr<JsonSerializer> create_json_serializer();

} // namespace shield::serialization