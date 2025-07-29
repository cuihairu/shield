#pragma once

#include "json_universal_serializer.hpp"
#include "messagepack_universal_serializer.hpp"
#include "protobuf_universal_serializer.hpp"
#include "universal_serializer.hpp"

namespace shield::serialization {

// Serialization system configuration
struct SerializationConfig {
    bool enable_json = true;
    bool enable_protobuf = true;
    bool enable_messagepack = true;
    bool enable_sproto = false;  // Not yet implemented

    // Default format selection strategy
    SerializationFormat default_format = SerializationFormat::JSON;

    // Performance options
    bool enable_auto_format_detection = true;
    bool enable_compression = false;  // May support compression in future
};

// Universal serialization system manager
class UniversalSerializationSystem {
public:
    static UniversalSerializationSystem &instance() {
        static UniversalSerializationSystem system;
        return system;
    }

    // Initialize serialization system
    void initialize(const SerializationConfig &config = SerializationConfig{});

    // Get registry reference
    SerializerRegistry &registry() { return SerializerRegistry::instance(); }

    // Check system status
    bool is_initialized() const { return initialized_; }
    const SerializationConfig &get_config() const { return config_; }

    // Get system information
    std::string get_system_info() const;
    std::vector<std::string> get_available_formats() const;

    // Convenience method: auto-select best serializer by type
    template <typename T>
    SerializationFormat get_recommended_format() const {
        if (config_.enable_auto_format_detection) {
            return detect_best_format<T>();
        } else {
            return config_.default_format;
        }
    }

    // Unified serialization interface
    template <typename T>
    auto serialize_auto(const T &object) {
        auto format = get_recommended_format<T>();
        return serialize_with_format<T>(object, format);
    }

    template <typename T>
    T deserialize_auto(const std::vector<uint8_t> &data,
                       SerializationFormat format) {
        return deserialize_with_format<T>(data, format);
    }

private:
    UniversalSerializationSystem() = default;
    ~UniversalSerializationSystem() = default;

    // Disable copy and move
    UniversalSerializationSystem(const UniversalSerializationSystem &) = delete;
    UniversalSerializationSystem &operator=(
        const UniversalSerializationSystem &) = delete;

    bool initialized_ = false;
    SerializationConfig config_;

    // Serialize with specified format
    template <typename T>
    auto serialize_with_format(const T &object, SerializationFormat format) {
        switch (format) {
            case SerializationFormat::JSON:
                if constexpr (JsonSerializable<T>) {
                    return serialize_as<SerializationFormat::JSON>(object);
                } else {
                    throw SerializationException(
                        "Type not compatible with JSON format");
                }
                break;
            case SerializationFormat::PROTOBUF:
                if constexpr (ProtobufSerializable<T>) {
                    return serialize_as<SerializationFormat::PROTOBUF>(object);
                } else {
                    throw SerializationException(
                        "Type not compatible with Protobuf format");
                }
                break;
            case SerializationFormat::MESSAGEPACK:
                if constexpr (MessagePackSerializable<T>) {
                    return serialize_as<SerializationFormat::MESSAGEPACK>(
                        object);
                } else {
                    throw SerializationException(
                        "Type not compatible with MessagePack format");
                }
                break;
            default:
                throw SerializationException(
                    "Unsupported serialization format");
        }
    }

    template <typename T>
    T deserialize_with_format(const auto &data, SerializationFormat format) {
        switch (format) {
            case SerializationFormat::JSON:
                if constexpr (JsonSerializable<T>) {
                    return deserialize_as<SerializationFormat::JSON, T>(data);
                } else {
                    throw SerializationException(
                        "Type not compatible with JSON format");
                }
                break;
            case SerializationFormat::PROTOBUF:
                if constexpr (ProtobufSerializable<T>) {
                    return deserialize_as<SerializationFormat::PROTOBUF, T>(
                        data);
                } else {
                    throw SerializationException(
                        "Type not compatible with Protobuf format");
                }
                break;
            case SerializationFormat::MESSAGEPACK:
                if constexpr (MessagePackSerializable<T>) {
                    return deserialize_as<SerializationFormat::MESSAGEPACK, T>(
                        data);
                } else {
                    throw SerializationException(
                        "Type not compatible with MessagePack format");
                }
                break;
            default:
                throw SerializationException(
                    "Unsupported serialization format");
        }
    }
};

// Global initialization function
void initialize_universal_serialization_system(
    const SerializationConfig &config = SerializationConfig{});

// Convenient global functions
template <typename T>
auto serialize_universal(const T &object) {
    return UniversalSerializationSystem::instance().serialize_auto(object);
}

template <typename T>
T deserialize_universal(const std::vector<uint8_t> &data,
                        SerializationFormat format) {
    return UniversalSerializationSystem::instance().deserialize_auto<T>(data,
                                                                        format);
}

}  // namespace shield::serialization