#include "shield/serialization/json_universal_serializer.hpp"
#include <algorithm>

namespace shield::serialization {

std::vector<uint8_t> JsonUniversalSerializer::serialize_bytes(const void* object, const std::type_info& type) {
    // This method is mainly used for type erasure scenarios
    // For JSON, we need a type registration mechanism or reflection to handle
    // Temporarily throw exception, suggesting users to use template methods
    throw SerializationException("Type-erased JSON serialization not yet implemented. Use template methods instead. Type: " + 
                                std::string(type.name()));
}

void JsonUniversalSerializer::deserialize_bytes(const std::vector<uint8_t>& data, void* object, const std::type_info& type) {
    // Same as above, type-erased deserialization requires additional type information
    throw SerializationException("Type-erased JSON deserialization not yet implemented. Use template methods instead. Type: " + 
                                std::string(type.name()));
}

std::unique_ptr<JsonUniversalSerializer> create_json_universal_serializer() {
    return std::make_unique<JsonUniversalSerializer>();
}

} // namespace shield::serialization