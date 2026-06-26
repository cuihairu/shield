// [SHIELD_TRANSPORT] Codec interface
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shield::transport {

/// @brief Codec for serializing/deserializing messages
class Codec {
public:
    virtual ~Codec() = default;

    /// @brief Encode a message to bytes
    virtual std::vector<uint8_t> encode(std::string_view method,
                                        std::string_view payload) = 0;

    /// @brief Decode bytes to method and payload
    virtual bool decode(const std::vector<uint8_t>& data, std::string& method,
                        std::string& payload) = 0;

    /// @brief Get codec name
    virtual std::string name() const = 0;
};

/// @brief JSON codec
class JsonCodec : public Codec {
public:
    std::vector<uint8_t> encode(std::string_view method,
                                std::string_view payload) override;

    bool decode(const std::vector<uint8_t>& data, std::string& method,
                std::string& payload) override;

    std::string name() const override { return "json"; }
};

/// @brief MessagePack codec
class MessagePackCodec : public Codec {
public:
    std::vector<uint8_t> encode(std::string_view method,
                                std::string_view payload) override;

    bool decode(const std::vector<uint8_t>& data, std::string& method,
                std::string& payload) override;

    std::string name() const override { return "msgpack"; }
};

/// @brief Protobuf codec (if available)
#ifdef SHIELD_HAS_PROTOBUF
class ProtobufCodec : public Codec {
public:
    std::vector<uint8_t> encode(std::string_view method,
                                std::string_view payload) override;

    bool decode(const std::vector<uint8_t>& data, std::string& method,
                std::string& payload) override;

    std::string name() const override { return "protobuf"; }
};
#endif

/// @brief Create codec by name
std::unique_ptr<Codec> create_codec(std::string_view name);

}  // namespace shield::transport
