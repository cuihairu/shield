#pragma once

#include "universal_serializer.hpp"
#include <google/protobuf/message.h>
#include <google/protobuf/message_lite.h>

namespace shield::serialization {

// Protobuf serializer implementation
class ProtobufUniversalSerializer
    : public UniversalSerializer<SerializationFormat::PROTOBUF> {
public:
  std::vector<uint8_t> serialize_bytes(const void *object,
                                       const std::type_info &type) override;
  void deserialize_bytes(const std::vector<uint8_t> &data, void *object,
                         const std::type_info &type) override;

  std::string get_name() const override { return "Protobuf"; }

protected:
  template <typename T> std::vector<uint8_t> serialize_impl(const T &object) {
    if constexpr (ProtobufSerializable<T>) {
      return serialize_protobuf(object);
    } else {
      throw SerializationException("Type is not Protobuf serializable");
    }
  }

  template <typename T> T deserialize_impl(const std::vector<uint8_t> &data) {
    if constexpr (ProtobufSerializable<T>) {
      return deserialize_protobuf<T>(data);
    } else {
      throw SerializationException("Type is not Protobuf deserializable");
    }
  }

private:
  // Protobuf serialization implementation
  template <typename T>
  std::vector<uint8_t> serialize_protobuf(const T &message) {
    try {
      if constexpr (std::is_base_of_v<google::protobuf::MessageLite, T>) {
        // Standard Protobuf MessageLite
        std::string serialized_data;
        if (!message.SerializeToString(&serialized_data)) {
          throw SerializationException("Failed to serialize Protobuf message");
        }
        return std::vector<uint8_t>(serialized_data.begin(),
                                    serialized_data.end());
      } else if constexpr (requires { message.SerializeToString(); }) {
        // Custom SerializeToString method
        std::string serialized_data = message.SerializeToString();
        return std::vector<uint8_t>(serialized_data.begin(),
                                    serialized_data.end());
      } else {
        throw SerializationException(
            "No Protobuf serialization method available for type: " +
            std::string(typeid(T).name()));
      }
    } catch (const std::exception &e) {
      throw SerializationException("Protobuf serialization failed: " +
                                   std::string(e.what()));
    }
  }

  template <typename T>
  T deserialize_protobuf(const std::vector<uint8_t> &data) {
    try {
      if constexpr (std::is_base_of_v<google::protobuf::MessageLite, T>) {
        // Standard Protobuf MessageLite
        T message;
        std::string data_str(data.begin(), data.end());
        if (!message.ParseFromString(data_str)) {
          throw SerializationException("Failed to parse Protobuf message");
        }
        return message;
      } else if constexpr (requires { T::ParseFromString(std::string{}); }) {
        // Static parsing method
        std::string data_str(data.begin(), data.end());
        return T::ParseFromString(data_str);
      } else if constexpr (std::is_default_constructible_v<T> &&
                           requires(T &obj) {
                             obj.ParseFromString(std::string{});
                           }) {
        // Instance parsing method
        T message;
        std::string data_str(data.begin(), data.end());
        if (!message.ParseFromString(data_str)) {
          throw SerializationException("Failed to parse Protobuf message");
        }
        return message;
      } else {
        throw SerializationException(
            "No Protobuf deserialization method available for type: " +
            std::string(typeid(T).name()));
      }
    } catch (const std::exception &e) {
      throw SerializationException("Protobuf deserialization failed: " +
                                   std::string(e.what()));
    }
  }
};

// Factory function
std::unique_ptr<ProtobufUniversalSerializer>
create_protobuf_universal_serializer();

// Convenience functions
template <typename T>
std::vector<uint8_t> to_protobuf_bytes(const T &message)
  requires ProtobufSerializable<T>
{
  return serialize_as<SerializationFormat::PROTOBUF>(message);
}

template <typename T>
T from_protobuf_bytes(const std::vector<uint8_t> &data)
  requires ProtobufSerializable<T>
{
  return deserialize_as<SerializationFormat::PROTOBUF, T>(data);
}

// String version convenience functions
template <typename T>
std::string to_protobuf_string(const T &message)
  requires ProtobufSerializable<T>
{
  auto bytes = to_protobuf_bytes(message);
  return std::string(bytes.begin(), bytes.end());
}

template <typename T>
T from_protobuf_string(const std::string &data)
  requires ProtobufSerializable<T>
{
  std::vector<uint8_t> bytes(data.begin(), data.end());
  return from_protobuf_bytes<T>(bytes);
}

} // namespace shield::serialization