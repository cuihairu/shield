// [SHIELD_TRANSPORT] Codec implementation
#include "shield/transport/codec.hpp"

#include <nlohmann/json.hpp>

#ifdef SHIELD_ENABLE_MESSAGEPACK
#include <msgpack.hpp>
#endif

namespace shield::transport {

// JsonCodec implementation
std::vector<uint8_t> JsonCodec::encode(std::string_view method,
                                       std::string_view payload) {
    nlohmann::json j;
    j["method"] = std::string(method);
    j["payload"] = std::string(payload);

    std::string str = j.dump();
    return std::vector<uint8_t>(str.begin(), str.end());
}

bool JsonCodec::decode(const std::vector<uint8_t>& data, std::string& method,
                       std::string& payload) {
    try {
        nlohmann::json j = nlohmann::json::parse(data);

        if (j.contains("method")) {
            method = j["method"].get<std::string>();
        }

        if (j.contains("payload")) {
            payload = j["payload"].get<std::string>();
        }

        return true;
    } catch (...) {
        return false;
    }
}

// MessagePackCodec implementation
#ifdef SHIELD_ENABLE_MESSAGEPACK
std::vector<uint8_t> MessagePackCodec::encode(std::string_view method,
                                              std::string_view payload) {
    msgpack::sbuffer sbuf;
    std::vector<std::string> values{std::string(method), std::string(payload)};
    msgpack::pack(sbuf, values);

    return std::vector<uint8_t>(sbuf.data(), sbuf.data() + sbuf.size());
}

bool MessagePackCodec::decode(const std::vector<uint8_t>& data,
                              std::string& method, std::string& payload) {
    try {
        msgpack::object_handle oh = msgpack::unpack(
            reinterpret_cast<const char*>(data.data()), data.size());

        auto obj = oh.get();
        if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 2) {
            return false;
        }

        method = obj.via.array.ptr[0].as<std::string>();
        payload = obj.via.array.ptr[1].as<std::string>();

        return true;
    } catch (...) {
        return false;
    }
}
#endif

// Factory function
std::unique_ptr<Codec> create_codec(std::string_view name) {
    if (name == "json") {
        return std::make_unique<JsonCodec>();
    }
#ifdef SHIELD_ENABLE_MESSAGEPACK
    if (name == "msgpack") {
        return std::make_unique<MessagePackCodec>();
    }
#endif
    return nullptr;
}

}  // namespace shield::transport
