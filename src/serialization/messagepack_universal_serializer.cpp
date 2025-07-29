#include "shield/serialization/messagepack_universal_serializer.hpp"
#include "shield/serialization/universal_serialization_system.hpp"
#include <typeindex>
#include <unordered_map>
#include <functional>

namespace shield::serialization {

// Type registry for MessagePack serialization
class MessagePackTypeRegistry {
public:
    using SerializeFunc = std::function<std::vector<uint8_t>(const void*)>;
    using DeserializeFunc = std::function<void(const std::vector<uint8_t>&, void*)>;
    
    template<typename T>
    void register_type() {
        if constexpr (MessagePackSerializable<T>) {
            std::type_index type_idx(typeid(T));
            
            serialize_funcs_[type_idx] = [](const void* obj) -> std::vector<uint8_t> {
                const T* typed_obj = static_cast<const T*>(obj);
                try {
                    msgpack::sbuffer buffer;
                    msgpack::pack(buffer, *typed_obj);
                    return std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size());
                } catch (const std::exception& e) {
                    throw SerializationException("MessagePack serialization failed: " + std::string(e.what()));
                }
            };
            
            deserialize_funcs_[type_idx] = [](const std::vector<uint8_t>& data, void* obj) {
                T* typed_obj = static_cast<T*>(obj);
                try {
                    auto handle = msgpack::unpack(reinterpret_cast<const char*>(data.data()), data.size());
                    handle.get().convert(*typed_obj);
                } catch (const std::exception& e) {
                    throw SerializationException("MessagePack deserialization failed: " + std::string(e.what()));
                }
            };
        }
    }
    
    std::vector<uint8_t> serialize(const void* object, const std::type_info& type) {
        std::type_index type_idx(type);
        auto it = serialize_funcs_.find(type_idx);
        if (it == serialize_funcs_.end()) {
            throw SerializationException("Type not registered for MessagePack serialization: " + std::string(type.name()));
        }
        return it->second(object);
    }
    
    void deserialize(const std::vector<uint8_t>& data, void* object, const std::type_info& type) {
        std::type_index type_idx(type);
        auto it = deserialize_funcs_.find(type_idx);
        if (it == deserialize_funcs_.end()) {
            throw SerializationException("Type not registered for MessagePack deserialization: " + std::string(type.name()));
        }
        it->second(data, object);
    }
    
    static MessagePackTypeRegistry& instance() {
        static MessagePackTypeRegistry registry;
        return registry;
    }
    
private:
    std::unordered_map<std::type_index, SerializeFunc> serialize_funcs_;
    std::unordered_map<std::type_index, DeserializeFunc> deserialize_funcs_;
};

std::vector<uint8_t> MessagePackUniversalSerializer::serialize_bytes(const void* object, const std::type_info& type) {
    return MessagePackTypeRegistry::instance().serialize(object, type);
}

void MessagePackUniversalSerializer::deserialize_bytes(const std::vector<uint8_t>& data, void* object, const std::type_info& type) {
    MessagePackTypeRegistry::instance().deserialize(data, object, type);
}

std::unique_ptr<MessagePackUniversalSerializer> create_messagepack_universal_serializer() {
    return std::make_unique<MessagePackUniversalSerializer>();
}

// Helper function to register common types
void register_common_messagepack_types() {
    auto& registry = MessagePackTypeRegistry::instance();
    
    // Register common built-in types
    registry.register_type<int>();
    registry.register_type<long>();
    registry.register_type<float>();
    registry.register_type<double>();
    registry.register_type<bool>();
    registry.register_type<std::string>();
    registry.register_type<std::vector<int>>();
    registry.register_type<std::vector<std::string>>();
    registry.register_type<std::map<std::string, std::string>>();
}

} // namespace shield::serialization