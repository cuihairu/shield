#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include "serialization_traits.hpp"

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
    virtual std::vector<uint8_t> serialize_bytes(
        const void *object, const std::type_info &type) = 0;
    virtual void deserialize_bytes(const std::vector<uint8_t> &data,
                                   void *object,
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
        if constexpr (Format == SerializationFormat::JSON) {
            try {
                nlohmann::json j = object;
                return j.dump();
            } catch (const std::exception &e) {
                throw SerializationException(std::string("JSON serialize failed: ") +
                                             e.what());
            }
        } else if constexpr (Format == SerializationFormat::MESSAGEPACK) {
            try {
                msgpack::sbuffer buffer;
                msgpack::pack(buffer, object);
                return std::vector<uint8_t>(
                    reinterpret_cast<const uint8_t *>(buffer.data()),
                    reinterpret_cast<const uint8_t *>(buffer.data()) +
                        buffer.size());
            } catch (const std::exception &e) {
                throw SerializationException(
                    std::string("MessagePack serialize failed: ") + e.what());
            }
        } else if constexpr (Format == SerializationFormat::PROTOBUF) {
            try {
                std::string out;
                if (!object.SerializeToString(&out)) {
                    throw SerializationException("Protobuf SerializeToString failed");
                }
                return std::vector<uint8_t>(out.begin(), out.end());
            } catch (const std::exception &e) {
                throw SerializationException(std::string("Protobuf serialize failed: ") +
                                             e.what());
            }
        } else {
            static_assert(Format == SerializationFormat::JSON ||
                              Format == SerializationFormat::MESSAGEPACK ||
                              Format == SerializationFormat::PROTOBUF,
                          "Unsupported serialization format");
        }
    }

    template <typename T>
    T deserialize(const serialization_result_t<Format> &data)
        requires Serializable<T, Format>
    {
        if constexpr (Format == SerializationFormat::JSON) {
            try {
                nlohmann::json j = nlohmann::json::parse(data);
                return j.template get<T>();
            } catch (const std::exception &e) {
                throw SerializationException(
                    std::string("JSON deserialize failed: ") + e.what());
            }
        } else if constexpr (Format == SerializationFormat::MESSAGEPACK) {
            try {
                auto handle = msgpack::unpack(
                    reinterpret_cast<const char *>(data.data()), data.size());
                T result{};
                handle.get().convert(result);
                return result;
            } catch (const std::exception &e) {
                throw SerializationException(
                    std::string("MessagePack deserialize failed: ") + e.what());
            }
        } else if constexpr (Format == SerializationFormat::PROTOBUF) {
            try {
                T result{};
                const std::string s(data.begin(), data.end());
                if (!result.ParseFromString(s)) {
                    throw SerializationException("Protobuf ParseFromString failed");
                }
                return result;
            } catch (const std::exception &e) {
                throw SerializationException(std::string("Protobuf deserialize failed: ") +
                                             e.what());
            }
        } else {
            static_assert(Format == SerializationFormat::JSON ||
                              Format == SerializationFormat::MESSAGEPACK ||
                              Format == SerializationFormat::PROTOBUF,
                          "Unsupported serialization format");
        }
    }

    // Convenience method: auto-detect and use best format
    template <typename T>
    static auto serialize_auto(const T &object) {
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
    std::unordered_map<SerializationFormat,
                       std::unique_ptr<IUniversalSerializer>>
        serializers_;
};

// Global serialization function - auto-select best format
template <typename T>
auto serialize(const T &object) {
    constexpr auto format = detect_best_format<T>();
    auto *serializer = SerializerRegistry::instance().get_serializer(format);
    if (!serializer) {
        throw SerializationException(
            "No serializer available for detected format");
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

}  // namespace shield::serialization
