#include "shield/serialization/serializer.hpp"
#include "shield/serialization/json_serializer_new.hpp"
#include "shield/core/logger.hpp"

namespace shield::serialization {

// Initialize the serialization system with built-in serializers
void initialize_serialization_system() {
    auto& registry = SerializerRegistry::instance();
    
    // Register JSON serializer
    registry.register_serializer(SerializationFormat::JSON, create_json_serializer());
    
    SHIELD_LOG_INFO << "Serialization system initialized with JSON support";
    
    // TODO: Add other serializers (Binary, Protobuf, MessagePack) as they are implemented
}

} // namespace shield::serialization