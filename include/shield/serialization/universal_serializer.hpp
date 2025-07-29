#pragma once

#include "serialization_traits.hpp"
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace shield::serialization {

// Serialization exception
class SerializationException : public std::runtime_error {
public:
  explicit SerializationException(const std::string &message)
      : std::runtime_error("Serialization error: " + message) {}
};

// Universal serializer interface
class IUniversalSerializer {
public:
  virtual ~IUniversalSerializer() = default;

  // Pure virtual functions, implemented by concrete serializers
  virtual std::vector<uint8_t> serialize_bytes(const void *object,
                                               const std::type_info &type) = 0;
  virtual void deserialize_bytes(const std::vector<uint8_t> &data, void *object,
                                 const std::type_info &type) = 0;

  virtual SerializationFormat get_format() const = 0;
  virtual std::string get_name() const = 0;
};

// Universal serializer template base class
template <SerializationFormat Format>
class UniversalSerializer : public IUniversalSerializer {
public:
  SerializationFormat get_format() const override { return Format; }

  // Type-safe template interface
  template <typename T>
  auto serialize(const T &object) -> serialization_result_t<Format>
    requires Serializable<T, Format>
  {
    // Call the virtual function with type info
    if constexpr (std::is_same_v<serialization_result_t<Format>, std::string>) {
      auto bytes = serialize_bytes(&object, typeid(T));
      return std::string(bytes.begin(), bytes.end());
    } else {
      return serialize_bytes(&object, typeid(T));
    }
  }

  template <typename T>
  T deserialize(const serialization_result_t<Format> &data)
    requires Serializable<T, Format>
  {
    T result;
    if constexpr (std::is_same_v<serialization_result_t<Format>, std::string>) {
      std::vector<uint8_t> bytes(data.begin(), data.end());
      deserialize_bytes(bytes, &result, typeid(T));
    } else {
      deserialize_bytes(data, &result, typeid(T));
    }
    return result;
  }

  // Convenience method: auto-detect and use best format
  template <typename T> static auto serialize_auto(const T &object) {
    constexpr auto best_format = detect_best_format<T>();
    if constexpr (best_format == Format) {
      return UniversalSerializer<Format>{}.serialize(object);
    } else {
      static_assert(best_format == Format,
                    "Type not compatible with this serializer");
    }
  }
};

// Serializer registry
class SerializerRegistry {
public:
  static SerializerRegistry &instance() {
    static SerializerRegistry registry;
    return registry;
  }

  // Register serializer
  void register_serializer(SerializationFormat format,
                           std::unique_ptr<IUniversalSerializer> serializer) {
    serializers_[format] = std::move(serializer);
  }

  // Get serializer
  IUniversalSerializer *get_serializer(SerializationFormat format) {
    auto it = serializers_.find(format);
    return (it != serializers_.end()) ? it->second.get() : nullptr;
  }

  // Check if format is supported
  bool supports_format(SerializationFormat format) const {
    return serializers_.find(format) != serializers_.end();
  }

  // Get list of supported formats
  std::vector<SerializationFormat> get_supported_formats() const {
    std::vector<SerializationFormat> formats;
    for (const auto &[format, _] : serializers_) {
      formats.push_back(format);
    }
    return formats;
  }

private:
  std::unordered_map<SerializationFormat, std::unique_ptr<IUniversalSerializer>>
      serializers_;
};

// Global serialization function - auto-select best format
template <typename T> auto serialize(const T &object) {
  constexpr auto format = detect_best_format<T>();
  auto *serializer = SerializerRegistry::instance().get_serializer(format);
  if (!serializer) {
    throw SerializationException("No serializer available for detected format");
  }

  if constexpr (format == SerializationFormat::JSON) {
    // JSON special handling, return string
    return static_cast<UniversalSerializer<format> *>(serializer)
        ->serialize(object);
  } else {
    // Other formats return byte array
    return static_cast<UniversalSerializer<format> *>(serializer)
        ->serialize(object);
  }
}

// Format-specific serialization function
template <SerializationFormat Format, typename T>
auto serialize_as(const T &object) -> serialization_result_t<Format>
  requires Serializable<T, Format>
{
  auto *serializer = SerializerRegistry::instance().get_serializer(Format);
  if (!serializer) {
    throw SerializationException(
        "No serializer available for specified format");
  }

  return static_cast<UniversalSerializer<Format> *>(serializer)
      ->serialize(object);
}

// Deserialization function
template <SerializationFormat Format, typename T>
T deserialize_as(const serialization_result_t<Format> &data)
  requires Serializable<T, Format>
{
  auto *serializer = SerializerRegistry::instance().get_serializer(Format);
  if (!serializer) {
    throw SerializationException(
        "No serializer available for specified format");
  }

  return static_cast<UniversalSerializer<Format> *>(serializer)
      ->template deserialize<T>(data);
}

} // namespace shield::serialization