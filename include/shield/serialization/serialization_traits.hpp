#pragma once

#include <concepts>
#include <msgpack.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <vector>

#ifndef SHIELD_HAS_PROTOBUF
#define SHIELD_HAS_PROTOBUF 0
#endif

#if SHIELD_HAS_PROTOBUF
#include <google/protobuf/message_lite.h>
#endif

namespace shield::serialization {

// Serialization format enumeration
enum class SerializationFormat { JSON, PROTOBUF, MESSAGEPACK, SPROTO, BINARY };

// Forward declarations
namespace detail {
template <typename T, SerializationFormat Format>
struct has_serialize_method;

template <typename T, SerializationFormat Format>
struct has_deserialize_method;
}  // namespace detail

// Core concept definitions
template <typename T>
concept JsonSerializable =
    requires(const T &t, const nlohmann::json &j) {
        // nlohmann::json conversion (covers built-ins + ADL serializers)
        { nlohmann::json(t) } -> std::same_as<nlohmann::json>;
        { j.template get<std::remove_cvref_t<T>>() }
            -> std::same_as<std::remove_cvref_t<T>>;
    };

template <typename T>
#if SHIELD_HAS_PROTOBUF
concept ProtobufSerializable =
    std::is_base_of_v<google::protobuf::MessageLite, std::remove_cvref_t<T>> ||
    requires(const T &t, std::remove_cvref_t<T> &obj, std::string &out,
             const std::string &data) {
        { t.SerializeToString(&out) } -> std::same_as<bool>;
        { obj.ParseFromString(data) } -> std::same_as<bool>;
    };
#else
concept ProtobufSerializable = false;
#endif

template <typename T>
concept MessagePackSerializable =
    requires(const T &t, std::remove_cvref_t<T> &obj,
             const std::vector<char> &data) {
        msgpack::pack(std::declval<msgpack::sbuffer &>(), t);
        msgpack::unpack(data.data(), data.size()).get().convert(obj);
    };

template <typename T>
concept SprotoSerializable =
    requires(const T &t, T &obj, const std::vector<uint8_t> &data) {
        // sproto support (assuming such interface exists)
        { t.encode() } -> std::convertible_to<std::vector<uint8_t>>;
        { T::decode(data) } -> std::convertible_to<T>;
    };

// Generic serialization concept
template <typename T, SerializationFormat Format>
concept Serializable =
    (Format == SerializationFormat::JSON && JsonSerializable<T>) ||
    (Format == SerializationFormat::PROTOBUF && ProtobufSerializable<T>) ||
    (Format == SerializationFormat::MESSAGEPACK &&
     MessagePackSerializable<T>) ||
    (Format == SerializationFormat::SPROTO && SprotoSerializable<T>);

// Serialization result type traits
template <SerializationFormat Format>
struct serialization_result_type {
    using type = std::vector<uint8_t>;  // Default binary
};

template <>
struct serialization_result_type<SerializationFormat::JSON> {
    using type = std::string;  // JSON uses string
};

template <SerializationFormat Format>
using serialization_result_t = typename serialization_result_type<Format>::type;

// Type detection utilities
template <typename T>
constexpr bool is_json_serializable_v = JsonSerializable<T>;

template <typename T>
constexpr bool is_protobuf_serializable_v = ProtobufSerializable<T>;

template <typename T>
constexpr bool is_messagepack_serializable_v = MessagePackSerializable<T>;

template <typename T>
constexpr bool is_sproto_serializable_v = SprotoSerializable<T>;

// Auto-detect best serialization format
template <typename T>
constexpr SerializationFormat detect_best_format() {
    if constexpr (std::same_as<std::remove_cvref_t<T>, nlohmann::json>) {
        return SerializationFormat::JSON;
    } else if constexpr (std::is_class_v<std::remove_cvref_t<T>> &&
                         !std::same_as<std::remove_cvref_t<T>, std::string> &&
                         JsonSerializable<T>) {
        // Prefer JSON for user-defined/class types when available, unless the
        // type is already "data-like" (e.g., std::string).
        return SerializationFormat::JSON;
    } else if constexpr (ProtobufSerializable<T>) {
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

}  // namespace shield::serialization
