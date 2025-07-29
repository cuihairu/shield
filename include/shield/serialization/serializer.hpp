#pragma once

#include <string>
#include <vector>
#include <memory>
#include <type_traits>
#include <stdexcept>
#include <unordered_map>

namespace shield::serialization {

// Serialization format enumeration
enum class SerializationFormat {
    JSON,
    BINARY,
    PROTOBUF,
    MSGPACK
};

// Base serializer interface
class ISerializer {
public:
    virtual ~ISerializer() = default;
    
    // Pure virtual methods for serialization/deserialization
    virtual std::vector<uint8_t> serialize_bytes(const void* object, const std::type_info& type) = 0;
    virtual void deserialize_bytes(const std::vector<uint8_t>& data, void* object, const std::type_info& type) = 0;
    
    // Convenience methods for string-based formats
    virtual std::string serialize_string(const void* object, const std::type_info& type) {
        auto bytes = serialize_bytes(object, type);
        return std::string(bytes.begin(), bytes.end());
    }
    
    virtual void deserialize_string(const std::string& data, void* object, const std::type_info& type) {
        std::vector<uint8_t> bytes(data.begin(), data.end());
        deserialize_bytes(bytes, object, type);
    }
    
    // Get the format this serializer handles
    virtual SerializationFormat get_format() const = 0;
};

// Template interface for type-safe serialization
template<typename T>
class TypedSerializer {
public:
    virtual ~TypedSerializer() = default;
    
    virtual std::vector<uint8_t> serialize(const T& object) = 0;
    virtual T deserialize(const std::vector<uint8_t>& data) = 0;
    
    // String-based convenience methods
    virtual std::string serialize_to_string(const T& object) {
        auto bytes = serialize(object);
        return std::string(bytes.begin(), bytes.end());
    }
    
    virtual T deserialize_from_string(const std::string& data) {
        std::vector<uint8_t> bytes(data.begin(), data.end());
        return deserialize(bytes);
    }
    
    virtual SerializationFormat get_format() const = 0;
};

// Serialization registry for managing different serializers
class SerializerRegistry {
public:
    static SerializerRegistry& instance() {
        static SerializerRegistry instance;
        return instance;
    }
    
    // Register a serializer for a specific format
    void register_serializer(SerializationFormat format, std::unique_ptr<ISerializer> serializer);
    
    // Get serializer for a specific format
    ISerializer* get_serializer(SerializationFormat format);
    
    // Template convenience methods
    template<typename T>
    std::vector<uint8_t> serialize(const T& object, SerializationFormat format = SerializationFormat::JSON) {
        auto* serializer = get_serializer(format);
        if (!serializer) {
            throw std::runtime_error("No serializer registered for format");
        }
        return serializer->serialize_bytes(&object, typeid(T));
    }
    
    template<typename T>
    T deserialize(const std::vector<uint8_t>& data, SerializationFormat format = SerializationFormat::JSON) {
        auto* serializer = get_serializer(format);
        if (!serializer) {
            throw std::runtime_error("No serializer registered for format");
        }
        T object;
        serializer->deserialize_bytes(data, &object, typeid(T));
        return object;
    }
    
    template<typename T>
    std::string serialize_to_string(const T& object, SerializationFormat format = SerializationFormat::JSON) {
        auto* serializer = get_serializer(format);
        if (!serializer) {
            throw std::runtime_error("No serializer registered for format");
        }
        return serializer->serialize_string(&object, typeid(T));
    }
    
    template<typename T>
    T deserialize_from_string(const std::string& data, SerializationFormat format = SerializationFormat::JSON) {
        auto* serializer = get_serializer(format);
        if (!serializer) {
            throw std::runtime_error("No serializer registered for format");
        }
        T object;
        serializer->deserialize_string(data, &object, typeid(T));
        return object;
    }

private:
    std::unordered_map<SerializationFormat, std::unique_ptr<ISerializer>> serializers_;
};

// Macro for easy serialization traits definition
#define SHIELD_SERIALIZABLE(Type) \
    template<> \
    struct is_serializable<Type> : std::true_type {}

// Type trait to check if a type is serializable
template<typename T>
struct is_serializable : std::false_type {};

} // namespace shield::serialization

// Forward declaration for initialization function
namespace shield::serialization {
    void initialize_serialization_system();
}