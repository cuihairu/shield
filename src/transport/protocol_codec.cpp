// [SHIELD_TRANSPORT] Body codecs: raw/passthrough/json providers, the
// plugin (external codec) bridge, the structured-body helpers and
// create_body_codec.
// Split out of protocol.cpp.
#include "shield/plugin/protocol_codec.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "shield/transport/protocol.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::transport {

namespace {
// Maximum nesting depth accepted for attacker-controlled JSON bodies. The
// nlohmann DOM parser is iterative and survives arbitrarily deep input, but
// every later walk over that DOM (dump(), json->lua conversion, structured
// route extraction) recurses on the C++ stack and crashes on deep nesting.
// Guard at the boundary so a deep frame never becomes a deep DOM.
constexpr std::size_t kMaxJsonDepth = 512;

// Non-recursive, string-aware depth pre-scan over raw JSON bytes. Returns
// true iff the span is structurally a JSON value that never nests deeper than
// max_depth. Braces inside quoted strings (including escaped quotes) are not
// counted. Conservative on malformed input: unterminated strings and
// unbalanced closers reject rather than guess.
bool json_nesting_within(std::string_view bytes, std::size_t max_depth) {
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : bytes) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
            if (depth > max_depth) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (depth == 0) {
                return false;  // unbalanced closer
            }
            --depth;
        }
    }
    return !in_string;  // unterminated string is not valid JSON
}

std::optional<BodyRouteKey> structured_route_key(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }

    BodyRouteKey key;
    if (value.contains("route_id") && value["route_id"].is_number_unsigned()) {
        key.route_id = value["route_id"].get<std::uint32_t>();
    } else if (value.contains("msg_id") &&
               value["msg_id"].is_number_unsigned()) {
        key.route_id = value["msg_id"].get<std::uint32_t>();
    }

    if (value.contains("route") && value["route"].is_string()) {
        key.route_name = value["route"].get<std::string>();
    } else if (value.contains("method") && value["method"].is_string()) {
        key.route_name = value["method"].get<std::string>();
    }

    if (key.empty()) {
        return std::nullopt;
    }
    return key;
}

DecodedBody decode_structured_body(PacketRef packet, const RouteEntry& route,
                                   nlohmann::json message) {
    DecodedBody body;
    body.route_id = route.route_id;
    body.bytes.assign(packet.body.begin(), packet.body.end());
    if (auto key = structured_route_key(message)) {
        body.route_name = std::move(key->route_name);
    }
    if (message.is_object() && message.contains("payload")) {
        body.message = message["payload"];
    } else {
        body.message = std::move(message);
    }
    return body;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

bool message_contains_route_hint(const nlohmann::json& message) {
    return message.is_object() &&
           (message.contains("route") || message.contains("route_id") ||
            message.contains("msg_id") || message.contains("method"));
}

// Whether the frame envelope carries the route id in the header. Header-route
// envelopes (idlen/typelen) make the body pure business data; envelopes
// without a header route (lenprefix/delimiter) need the {route, route_id,
// payload} wrapper below — it is the only way their frames stay routable.
bool envelope_carries_route(const ProtocolProfile& profile) {
    return profile.envelope_kind == EnvelopeKind::IdLen ||
           profile.envelope_kind == EnvelopeKind::TypeLen;
}

nlohmann::json encode_structured_message(const DecodedBody& body,
                                         const RouteEntry& route,
                                         const ProtocolProfile& profile) {
    if (envelope_carries_route(profile)) {
        // Terminal contract: the route travels in the frame header only —
        // never write route fields into the body.
        if (body.has_message()) {
            return *body.message;
        }
        if (!body.bytes.empty()) {
            throw std::runtime_error(
                "structured body codec expects business message, not raw "
                "bytes");
        }
        return nlohmann::json::object();
    }

    nlohmann::json message;
    if (body.has_message()) {
        if (message_contains_route_hint(*body.message)) {
            message = *body.message;
        } else {
            message = nlohmann::json::object();
            if (!body.route_name.empty()) {
                message["route"] = body.route_name;
            } else if (!route.debug_name.empty()) {
                message["route"] = route.debug_name;
            }
            if (route.route_id != 0) {
                message["route_id"] = route.route_id;
            }
            message["payload"] = *body.message;
        }
        return message;
    }

    if (!body.bytes.empty()) {
        throw std::runtime_error(
            "structured body codec expects business message, not raw bytes");
    }

    message = nlohmann::json::object();
    if (!body.route_name.empty()) {
        message["route"] = body.route_name;
    } else if (!route.debug_name.empty()) {
        message["route"] = route.debug_name;
    }
    if (route.route_id != 0) {
        message["route_id"] = route.route_id;
    }
    message["payload"] = nlohmann::json::object();
    return message;
}

nlohmann::json plugin_business_message(const DecodedBody& body) {
    if (body.has_message()) {
        const auto& message = *body.message;
        if (message.is_object() && message.contains("payload") &&
            message_contains_route_hint(message)) {
            return message["payload"];
        }
        return message;
    }

    if (!body.bytes.empty()) {
        throw std::runtime_error(
            "plugin body codec expects business message, not raw bytes");
    }

    return nlohmann::json::object();
}

std::string plugin_error_message(const shield_error_v1& err,
                                 std::string_view fallback) {
    if (err.message && err.message[0] != '\0') {
        return err.message;
    }
    if (err.code && err.code[0] != '\0') {
        return err.code;
    }
    return std::string(fallback);
}
}  // namespace

std::optional<BodyRouteKey> BodyCodec::route_key(PacketRef) {
    return std::nullopt;
}

DecodedBody RawBodyCodec::decode(PacketRef packet, const RouteEntry& route) {
    DecodedBody body;
    body.route_id = route.route_id;
    body.bytes.assign(packet.body.begin(), packet.body.end());
    return body;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

std::vector<std::uint8_t> RawBodyCodec::encode(const DecodedBody& body,
                                               const RouteEntry& route,
                                               const ProtocolProfile&) {
    (void)route;
    if (body.has_message()) {
        throw std::runtime_error("raw body codec expects bytes");
    }
    return body.bytes;
}

PassthroughBodyCodec::PassthroughBodyCodec(std::string name)
    : name_(std::move(name)) {}

DecodedBody PassthroughBodyCodec::decode(PacketRef packet,
                                         const RouteEntry& route) {
    (void)packet;
    (void)route;
    throw std::runtime_error("body codec does not support decode_local");
}

std::vector<std::uint8_t> PassthroughBodyCodec::encode(
    const DecodedBody& body, const RouteEntry& route,
    const ProtocolProfile& profile) {
    (void)body;
    (void)route;
    (void)profile;
    throw std::runtime_error("body codec does not support encode");
}

std::optional<BodyRouteKey> JsonBodyCodec::route_key(PacketRef packet) {
    try {
        const std::string_view bytes(
            reinterpret_cast<const char*>(packet.body.data()),
            packet.body.size());
        if (!json_nesting_within(bytes, kMaxJsonDepth)) {
            return std::nullopt;  // reject deep nesting before building a DOM
        }
        const auto json =
            nlohmann::json::parse(packet.body.begin(), packet.body.end());
        return structured_route_key(json);
    } catch (...) {
        return std::nullopt;
    }
}

DecodedBody JsonBodyCodec::decode(PacketRef packet, const RouteEntry& route) {
    const std::string_view bytes(
        reinterpret_cast<const char*>(packet.body.data()), packet.body.size());
    if (!json_nesting_within(bytes, kMaxJsonDepth)) {
        throw std::runtime_error("json body exceeds maximum nesting depth");
    }
    auto json = nlohmann::json::parse(packet.body.begin(), packet.body.end());
    return decode_structured_body(packet, route, std::move(json));
}

std::vector<std::uint8_t> JsonBodyCodec::encode(
    const DecodedBody& body, const RouteEntry& route,
    const ProtocolProfile& profile) {
    const auto json = encode_structured_message(body, route, profile);
    const auto serialized = json.dump();
    return std::vector<std::uint8_t>(serialized.begin(), serialized.end());
}

std::unique_ptr<BodyCodec> create_body_codec(std::string_view name) {
    if (name == "raw") {
        return std::make_unique<RawBodyCodec>();
    }
    if (name == "json") {
        return std::make_unique<JsonBodyCodec>();
    }
    if (name == "msgpack" || name == "protobuf" || name == "fbs" ||
        name == "flatbuffers") {
        return std::make_unique<PassthroughBodyCodec>(std::string(name));
    }
    if (name == "xmldef" || name == "xml_def") {
        return std::make_unique<PassthroughBodyCodec>("xmldef");
    }
    return nullptr;
}

ExternalBodyCodec::ExternalBodyCodec(std::string provider, std::string name,
                                     const shield_protocol_codec_v1* codec)
    : provider_(std::move(provider)), name_(std::move(name)), codec_(codec) {}

DecodedBody ExternalBodyCodec::decode(PacketRef packet,
                                      const RouteEntry& route) {
    if (codec_ == nullptr || codec_->decode == nullptr) {
        throw std::runtime_error("protocol codec provider '" + provider_ +
                                 "' is not available");
    }

    shield_protocol_decode_args_v1 args{};
    args.route_name =
        route.schema_name.empty() ? nullptr : route.schema_name.c_str();
    args.payload = packet.body.data();
    args.payload_size = packet.body.size();

    shield_protocol_decode_result_v1 out{};
    shield_error_v1 err{};
    const int rc = codec_->decode(codec_, &args, &out, &err);
    if (rc != 0) {
        throw std::runtime_error(
            plugin_error_message(err, "protocol decode failed"));
    }

    try {
        DecodedBody body;  // GCOVR_EXCL_BR_LINE (compiler artifact: eh edge of
                           // a non-throwing default construction)
        body.route_id = route.route_id;
        body.route_name = route.debug_name;
        body.bytes.assign(packet.body.begin(), packet.body.end());

        if (out.message_json != nullptr && out.message_json_size > 0) {
            const auto size = static_cast<std::size_t>(out.message_json_size);
            body.message = nlohmann::json::parse(out.message_json,
                                                 out.message_json + size);
        } else {
            body.message = nlohmann::json::object();
        }

        if (codec_->free_decode_result) {
            codec_->free_decode_result(codec_, &out);
        }
        return body;  // GCOVR_EXCL_BR_LINE (compiler artifact: eh edge on
                      // return copy)
    } catch (...) {
        if (codec_->free_decode_result) {
            codec_->free_decode_result(codec_, &out);
        }
        throw;
    }
}

std::vector<std::uint8_t> ExternalBodyCodec::encode(const DecodedBody& body,
                                                    const RouteEntry& route,
                                                    const ProtocolProfile&) {
    if (codec_ == nullptr || codec_->encode == nullptr) {
        throw std::runtime_error("protocol codec provider '" + provider_ +
                                 "' is not available");
    }

    const auto message = plugin_business_message(body);
    const auto message_text = message.dump();

    shield_protocol_encode_args_v1 args{};
    args.route_name =
        route.schema_name.empty() ? nullptr : route.schema_name.c_str();
    args.message_json = message_text.data();
    args.message_json_size = message_text.size();

    shield_protocol_encode_result_v1 out{};
    shield_error_v1 err{};
    const int rc = codec_->encode(codec_, &args, &out, &err);
    if (rc != 0) {
        throw std::runtime_error(
            plugin_error_message(err, "protocol encode failed"));
    }

    std::vector<std::uint8_t> payload;
    try {
        if (out.payload != nullptr && out.payload_size > 0) {
            const auto size = static_cast<std::size_t>(out.payload_size);
            payload.assign(out.payload, out.payload + size);
        }
        if (codec_->free_encode_result) {
            codec_->free_encode_result(codec_, &out);
        }
        return payload;
    } catch (...) {
        if (codec_->free_encode_result) {
            codec_->free_encode_result(codec_, &out);
        }
        throw;
    }
}

}  // namespace shield::transport
