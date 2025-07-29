#include "shield/serialization/protobuf_universal_serializer.hpp"

namespace shield::serialization {

std::vector<uint8_t> ProtobufUniversalSerializer::serialize_bytes(const void* object, const std::type_info& type) {
    // For Protobuf, we can try to convert the object to MessageLite
    if (const auto* message = static_cast<const google::protobuf::MessageLite*>(object)) {
        std::string serialized_data;
        if (!message->SerializeToString(&serialized_data)) {
            throw SerializationException("Failed to serialize Protobuf message of type: " + 
                                       std::string(type.name()));
        }
        return std::vector<uint8_t>(serialized_data.begin(), serialized_data.end());
    } else {
        throw SerializationException("Object is not a Protobuf MessageLite. Type: " + 
                                   std::string(type.name()));
    }
}

void ProtobufUniversalSerializer::deserialize_bytes(const std::vector<uint8_t>& data, void* object, const std::type_info& type) {
    if (auto* message = static_cast<google::protobuf::MessageLite*>(object)) {
        std::string data_str(data.begin(), data.end());
        if (!message->ParseFromString(data_str)) {
            throw SerializationException("Failed to parse Protobuf message of type: " + 
                                       std::string(type.name()));
        }
    } else {
        throw SerializationException("Object is not a Protobuf MessageLite. Type: " + 
                                   std::string(type.name()));
    }
}

std::unique_ptr<ProtobufUniversalSerializer> create_protobuf_universal_serializer() {
    return std::make_unique<ProtobufUniversalSerializer>();
}

} // namespace shield::serialization