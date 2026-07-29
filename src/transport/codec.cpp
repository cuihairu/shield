// [SHIELD_TRANSPORT] Codec implementation
#include "shield/transport/codec.hpp"

#include <nlohmann/json.hpp>

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

// Factory function
std::unique_ptr<Codec> create_codec(std::string_view name) {
    if (name == "json") {
        return std::make_unique<JsonCodec>();
    }
    return nullptr;
}

}  // namespace shield::transport
