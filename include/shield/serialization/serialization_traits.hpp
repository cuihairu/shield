#pragma once

#include <type_traits>
#include <string>
#include <vector>
#include <concepts>
#include <nlohmann/json.hpp>
#include <google/protobuf/message_lite.h>
#include <msgpack.hpp>

namespace shield::serialization {

// Serialization format enumeration
enum class SerializationFormat {
    JSON,
    PROTOBUF,
    MESSAGEPACK,
    SPROTO,
    BINARY
};

// Forward declarations
namespace detail {
    template<typename T, SerializationFormat Format>
    struct has_serialize_method;
    
    template<typename T, SerializationFormat Format>
    struct has_deserialize_method;
}

// Core concept definitions
template<typename T>
concept JsonSerializable = requires(const T& t, T& obj, const std::string& json_str) {
    // Support to_json/from_json ADL
    to_json(std::declval<nlohmann::json&>(), t);
    from_json(std::declval<const nlohmann::json&>(), obj);
} || requires(const T& t, const std::string& json_str) {
    // Or support member functions
    { t.to_json() } -> std::convertible_to<std::string>;
    { T::from_json(json_str) } -> std::convertible_to<T>;
};

template<typename T>
concept ProtobufSerializable = requires(const T& t, T& obj, const std::string& data) {
    // Protobuf MessageLite interface
    { t.SerializeToString() } -> std::convertible_to<std::string>;
    { obj.ParseFromString(data) } -> std::convertible_to<bool>;
} || requires(const T& t) {
    // Or check if inherits from google::protobuf::Message
    std::is_base_of_v<google::protobuf::MessageLite, T>;
};

template<typename T>
concept MessagePackSerializable = requires(const T& t, T& obj, const std::vector<char>& data) {
    // MessagePack support
    msgpack::pack(t);
    msgpack::unpack(data.data(), data.size()).get().convert(obj);
} || requires(const T& t) {
    // Or has custom msgpack adapter
    typename msgpack::adaptor::convert<T>;
};

template<typename T>
concept SprotoSerializable = requires(const T& t, T& obj, const std::vector<uint8_t>& data) {
    // sproto support (assuming such interface exists)
    { t.encode() } -> std::convertible_to<std::vector<uint8_t>>;
    { T::decode(data) } -> std::convertible_to<T>;
};

// Generic serialization concept
template<typename T, SerializationFormat Format>
concept Serializable = 
    (Format == SerializationFormat::JSON && JsonSerializable<T>) ||
    (Format == SerializationFormat::PROTOBUF && ProtobufSerializable<T>) ||
    (Format == SerializationFormat::MESSAGEPACK && MessagePackSerializable<T>) ||
    (Format == SerializationFormat::SPROTO && SprotoSerializable<T>);

// Serialization result type traits
template<SerializationFormat Format>
struct serialization_result_type {
    using type = std::vector<uint8_t>; // Default binary
};

template<>
struct serialization_result_type<SerializationFormat::JSON> {
    using type = std::string; // JSON uses string
};

template<SerializationFormat Format>
using serialization_result_t = typename serialization_result_type<Format>::type;

// Type detection utilities
template<typename T>
constexpr bool is_json_serializable_v = JsonSerializable<T>;

template<typename T>
constexpr bool is_protobuf_serializable_v = ProtobufSerializable<T>;

template<typename T>
constexpr bool is_messagepack_serializable_v = MessagePackSerializable<T>;

template<typename T>
constexpr bool is_sproto_serializable_v = SprotoSerializable<T>;

// Auto-detect best serialization format
template<typename T>
constexpr SerializationFormat detect_best_format() {
    if constexpr (ProtobufSerializable<T>) {
        return SerializationFormat::PROTOBUF;
    } else if constexpr (MessagePackSerializable<T>) {
        return SerializationFormat::MESSAGEPACK;
    } else if constexpr (SprotoSerializable<T>) {
        return SerializationFormat::SPROTO;
    } else if constexpr (JsonSerializable<T>) {
        return SerializationFormat::JSON;
    } else {
        return SerializationFormat::BINARY;
    }
}

} // namespace shield::serialization