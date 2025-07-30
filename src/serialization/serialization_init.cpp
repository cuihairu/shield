#include "shield/log/logger.hpp"
#include "shield/serialization/universal_serialization_system.hpp"

namespace shield::serialization {

// Initialize the serialization system with universal serializers
void initialize_serialization_system() {
    SHIELD_LOG_INFO << "Initializing universal serialization system...";

    // Configure serialization options
    SerializationConfig config;
    config.enable_json = true;
    config.enable_protobuf = true;
    config.enable_messagepack = true;
    config.enable_sproto = false;  // Not implemented yet
    config.default_format = SerializationFormat::JSON;
    config.enable_auto_format_detection = true;

    // Initialize the universal serialization system
    initialize_universal_serialization_system(config);

    SHIELD_LOG_INFO
        << "Universal serialization system initialized successfully";

    // Log available formats
    auto& system = UniversalSerializationSystem::instance();
    auto available_formats = system.get_available_formats();
    SHIELD_LOG_INFO << "Available serialization formats: ";
    for (const auto& format : available_formats) {
        SHIELD_LOG_INFO << "  - " << format;
    }
}

}  // namespace shield::serialization