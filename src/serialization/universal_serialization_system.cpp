#include "shield/serialization/universal_serialization_system.hpp"

#include "shield/log/logger.hpp"

namespace shield::serialization {

void UniversalSerializationSystem::initialize(
    const SerializationConfig& config) {
    if (initialized_) {
        SHIELD_LOG_WARN << "Serialization system already initialized";
        return;
    }

    config_ = config;
    auto& registry = SerializerRegistry::instance();

    try {
        // Register JSON serializer
        if (config.enable_json) {
            registry.register_serializer(SerializationFormat::JSON,
                                         create_json_universal_serializer());
            SHIELD_LOG_INFO << "JSON serializer registered";
        }

        // Register Protobuf serializer
        if (config.enable_protobuf) {
            registry.register_serializer(
                SerializationFormat::PROTOBUF,
                create_protobuf_universal_serializer());
            SHIELD_LOG_INFO << "Protobuf serializer registered";
        }

        // Register MessagePack serializer
        if (config.enable_messagepack) {
            registry.register_serializer(
                SerializationFormat::MESSAGEPACK,
                create_messagepack_universal_serializer());
            SHIELD_LOG_INFO << "MessagePack serializer registered";
        }

        // TODO: Register sproto serializer
        if (config.enable_sproto) {
            SHIELD_LOG_WARN << "sproto serializer not yet implemented";
        }

        initialized_ = true;
        SHIELD_LOG_INFO
            << "Universal serialization system initialized successfully";
        SHIELD_LOG_INFO << get_system_info();

    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to initialize serialization system: "
                         << e.what();
        throw SerializationException(
            "Serialization system initialization failed: " +
            std::string(e.what()));
    }
}

std::string UniversalSerializationSystem::get_system_info() const {
    std::ostringstream oss;
    oss << "Universal Serialization System Status:\n";
    oss << "  Initialized: " << (initialized_ ? "Yes" : "No") << "\n";
    oss << "  Default Format: ";

    switch (config_.default_format) {
        case SerializationFormat::JSON:
            oss << "JSON";
            break;
        case SerializationFormat::PROTOBUF:
            oss << "Protobuf";
            break;
        case SerializationFormat::MESSAGEPACK:
            oss << "MessagePack";
            break;
        case SerializationFormat::SPROTO:
            oss << "sproto";
            break;
        default:
            oss << "Unknown";
            break;
    }
    oss << "\n";

    oss << "  Auto Format Detection: "
        << (config_.enable_auto_format_detection ? "Enabled" : "Disabled")
        << "\n";
    oss << "  Available Formats: ";

    auto formats = get_available_formats();
    for (size_t i = 0; i < formats.size(); ++i) {
        oss << formats[i];
        if (i < formats.size() - 1) oss << ", ";
    }

    return oss.str();
}

std::vector<std::string> UniversalSerializationSystem::get_available_formats()
    const {
    std::vector<std::string> format_names;
    auto& registry = SerializerRegistry::instance();

    auto supported_formats = registry.get_supported_formats();
    for (auto format : supported_formats) {
        switch (format) {
            case SerializationFormat::JSON:
                format_names.push_back("JSON");
                break;
            case SerializationFormat::PROTOBUF:
                format_names.push_back("Protobuf");
                break;
            case SerializationFormat::MESSAGEPACK:
                format_names.push_back("MessagePack");
                break;
            case SerializationFormat::SPROTO:
                format_names.push_back("sproto");
                break;
            case SerializationFormat::BINARY:
                format_names.push_back("Binary");
                break;
            default:
                format_names.push_back("Unknown");
                break;
        }
    }

    return format_names;
}

void initialize_universal_serialization_system(
    const SerializationConfig& config) {
    UniversalSerializationSystem::instance().initialize(config);
}

}  // namespace shield::serialization