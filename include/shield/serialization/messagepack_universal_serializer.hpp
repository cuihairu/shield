#pragma once

#include "universal_serializer.hpp"
#include <msgpack.hpp>

namespace shield::serialization {

// MessagePack serializer implementation
class MessagePackUniversalSerializer
    : public UniversalSerializer<SerializationFormat::MESSAGEPACK> {
public:
  std::vector<uint8_t> serialize_bytes(const void *object,
                                       const std::type_info &type) override;
  void deserialize_bytes(const std::vector<uint8_t> &data, void *object,
                         const std::type_info &type) override;

  std::string get_name() const override { return "MessagePack"; }

protected:
  template <typename T> std::vector<uint8_t> serialize_impl(const T &object) {
    if constexpr (MessagePackSerializable<T>) {
      return serialize_messagepack(object);
    } else {
      throw SerializationException("Type is not MessagePack serializable");
    }
  }

  template <typename T> T deserialize_impl(const std::vector<uint8_t> &data) {
    if constexpr (MessagePackSerializable<T>) {
      return deserialize_messagepack<T>(data);
    } else {
      throw SerializationException("Type is not MessagePack deserializable");
    }
  }

private:
  // MessagePack serialization implementation
  template <typename T>
  std::vector<uint8_t> serialize_messagepack(const T &object) {
    try {
      if constexpr (requires { msgpack::pack(object); }) {
        // Use msgpack::pack
        msgpack::sbuffer buffer;
        msgpack::pack(buffer, object);
        return std::vector<uint8_t>(buffer.data(),
                                    buffer.data() + buffer.size());
      } else if constexpr (requires { object.msgpack_pack(); }) {
        // Custom msgpack_pack method
        return object.msgpack_pack();
      } else {
        // Try using msgpack object adapter
        msgpack::sbuffer buffer;
        msgpack::packer<msgpack::sbuffer> packer(buffer);
        packer.pack(object);
        return std::vector<uint8_t>(buffer.data(),
                                    buffer.data() + buffer.size());
      }
    } catch (const std::exception &e) {
      throw SerializationException("MessagePack serialization failed: " +
                                   std::string(e.what()));
    }
  }

  template <typename T>
  T deserialize_messagepack(const std::vector<uint8_t> &data) {
    try {
      // Use standard msgpack::unpack
      auto handle = msgpack::unpack(reinterpret_cast<const char *>(data.data()),
                                    data.size());
      T object;
      handle.get().convert(object);
      return object;
    } catch (const std::exception &e) {
      throw SerializationException("MessagePack deserialization failed: " +
                                   std::string(e.what()));
    }
  }
};

// Factory function
std::unique_ptr<MessagePackUniversalSerializer>
create_messagepack_universal_serializer();

// Helper function to register common types
void register_common_messagepack_types();

// Convenience functions
template <typename T>
std::vector<uint8_t> to_messagepack_bytes(const T &object)
  requires MessagePackSerializable<T>
{
  return serialize_as<SerializationFormat::MESSAGEPACK>(object);
}

template <typename T>
T from_messagepack_bytes(const std::vector<uint8_t> &data)
  requires MessagePackSerializable<T>
{
  return deserialize_as<SerializationFormat::MESSAGEPACK, T>(data);
}

} // namespace shield::serialization