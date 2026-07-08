// [SHIELD_TRANSPORT] Header-first protocol routing primitives
#include "shield/transport/protocol.hpp"

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

#include "shield/plugin/protocol_codec.h"

namespace shield::transport {
namespace {

bool valid_integer_width(std::uint8_t width) {
    return width == 1 || width == 2 || width == 4 || width == 8;
}

std::uint64_t read_uint(ByteSpan data, Endian endian) {
    std::uint64_t value = 0;

    if (endian == Endian::Big) {
        for (auto byte : data) {
            value = (value << 8) | byte;
        }
        return value;
    }

    for (std::size_t i = 0; i < data.size(); ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

void write_uint(std::uint64_t value, std::uint8_t width, Endian endian,
                std::vector<std::uint8_t>& out) {
    if (endian == Endian::Big) {
        for (std::size_t i = 0; i < width; ++i) {
            const auto shift = static_cast<unsigned>((width - 1 - i) * 8);
            out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
        }
        return;
    }

    for (std::size_t i = 0; i < width; ++i) {
        const auto shift = static_cast<unsigned>(i * 8);
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
}

bool fits_width(std::uint64_t value, std::uint8_t width) {
    if (width >= 8) {
        return true;
    }
    return value < (std::uint64_t{1} << (width * 8));
}

std::optional<std::size_t> body_length_from_field(std::uint64_t length_field,
                                                  std::size_t header_size,
                                                  bool includes_header) {
    if (!includes_header) {
        if (length_field > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(length_field);
    }

    if (length_field < header_size ||
        length_field > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(length_field) - header_size;
}

std::string trim_copy(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    try {
        std::size_t parsed = 0;
        const auto text = trim_copy(value);
        const auto number = std::stoull(text, &parsed, 0);
        if (parsed != text.size() ||
            number > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(number);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint16_t> parse_u16(std::string_view value) {
    const auto parsed = parse_u32(value);
    if (!parsed || *parsed > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

std::optional<bool> parse_bool(std::string_view value) {
    const auto text = trim_copy(value);
    if (text == "true" || text == "1" || text == "yes") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<RouteAction> parse_action(std::string_view value) {
    const auto text = trim_copy(value);
    if (text == "decode" || text == "decode_local") {
        return RouteAction::DecodeLocal;
    }
    if (text == "forward" || text == "forward_raw") {
        return RouteAction::ForwardRaw;
    }
    if (text == "drop") {
        return RouteAction::Drop;
    }
    return std::nullopt;
}

std::unordered_map<std::string, std::string> parse_xml_attributes(
    std::string_view tag) {
    std::unordered_map<std::string, std::string> attrs;
    std::size_t pos = 0;

    while (pos < tag.size()) {
        while (pos < tag.size() &&
               (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '\r' ||
                tag[pos] == '\n' || tag[pos] == '/')) {
            ++pos;
        }
        const auto key_begin = pos;
        while (pos < tag.size() &&
               (std::isalnum(static_cast<unsigned char>(tag[pos])) ||
                tag[pos] == '_' || tag[pos] == '-' || tag[pos] == '.')) {
            ++pos;
        }
        if (key_begin == pos) {
            ++pos;
            continue;
        }

        const auto key = std::string(tag.substr(key_begin, pos - key_begin));
        while (pos < tag.size() &&
               (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '\r' ||
                tag[pos] == '\n')) {
            ++pos;
        }
        if (pos >= tag.size() || tag[pos] != '=') {
            continue;
        }
        ++pos;
        while (pos < tag.size() &&
               (tag[pos] == ' ' || tag[pos] == '\t' || tag[pos] == '\r' ||
                tag[pos] == '\n')) {
            ++pos;
        }
        if (pos >= tag.size() || (tag[pos] != '"' && tag[pos] != '\'')) {
            continue;
        }

        const auto quote = tag[pos++];
        const auto value_begin = pos;
        while (pos < tag.size() && tag[pos] != quote) {
            ++pos;
        }
        if (pos >= tag.size()) {
            break;
        }
        attrs.insert_or_assign(
            key, std::string(tag.substr(value_begin, pos - value_begin)));
        ++pos;
    }

    return attrs;
}

const std::string* find_attr(
    const std::unordered_map<std::string, std::string>& attrs,
    std::string_view key) {
    const auto it = attrs.find(std::string(key));
    if (it == attrs.end()) {
        return nullptr;
    }
    return &it->second;
}

std::optional<Endian> parse_protocol_endian(const nlohmann::json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto text = value.get<std::string>();
    if (text == "big" || text == "be") {
        return Endian::Big;
    }
    if (text == "little" || text == "le") {
        return Endian::Little;
    }
    return std::nullopt;
}

std::optional<EnvelopeKind> parse_envelope_kind(const std::string& value) {
    if (value == "lenprefix" || value == "len-prefix" ||
        value == "len_prefix") {
        return EnvelopeKind::LenPrefix;
    }
    if (value == "idlen" || value == "id-len" || value == "id_len") {
        return EnvelopeKind::IdLen;
    }
    if (value == "typed_len" || value == "type_len" ||
        value == "typed-len" || value == "typelen") {
        return EnvelopeKind::TypeLen;
    }
    if (value == "delimiter" || value == "line") {
        return EnvelopeKind::Delimiter;
    }
    return std::nullopt;
}

std::optional<RouteSource> parse_route_source(const std::string& value) {
    if (value == "header" || value == "header.route_id" ||
        value == "header.msg_id") {
        return RouteSource::Header;
    }
    if (value == "body" || value == "body.route" ||
        value == "body.route_id") {
        return RouteSource::Body;
    }
    if (value == "none") {
        return RouteSource::None;
    }
    return std::nullopt;
}

RouteSource default_route_source_for_envelope(EnvelopeKind kind) {
    switch (kind) {
        case EnvelopeKind::IdLen:
        case EnvelopeKind::TypeLen:
            return RouteSource::Header;
        case EnvelopeKind::LenPrefix:
        case EnvelopeKind::Delimiter:
            return RouteSource::Body;
    }
    return RouteSource::Body;
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
    body.codec_id = route.codec_id;
    body.schema_id = route.schema_id;
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
}

bool message_contains_route_hint(const nlohmann::json& message) {
    return message.is_object() &&
           (message.contains("route") || message.contains("route_id") ||
            message.contains("msg_id") || message.contains("method"));
}

nlohmann::json encode_structured_message(const DecodedBody& body,
                                        const RouteEntry& route) {
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

PacketRef Packet::ref() const {
    return PacketRef{
        .route_id = route_id,
        .kind = kind,
        .flags = flags,
        .seq = seq,
        .body = body,
        .raw_frame = raw_frame,
    };
}

bool RouteTable::add(RouteEntry entry) {
    if (!entry.debug_name.empty() &&
        route_names_.contains(entry.debug_name)) {
        return false;
    }

    const auto route_id = entry.route_id;
    const auto route_name = entry.debug_name;
    const auto [_, inserted] = routes_.emplace(route_id, std::move(entry));
    if (inserted && !route_name.empty()) {
        route_names_.emplace(route_name, route_id);
    }
    return inserted;
}

void RouteTable::upsert(RouteEntry entry) {
    const auto route_id = entry.route_id;
    const auto route_name = entry.debug_name;

    const auto existing = routes_.find(route_id);
    if (existing != routes_.end() && !existing->second.debug_name.empty()) {
        const auto name = existing->second.debug_name;
        const auto name_it = route_names_.find(name);
        if (name_it != route_names_.end() && name_it->second == route_id) {
            route_names_.erase(name_it);
        }
    }

    routes_.insert_or_assign(route_id, std::move(entry));
    if (!route_name.empty()) {
        route_names_.insert_or_assign(route_name, route_id);
    }
}

const RouteEntry* RouteTable::find(std::uint32_t route_id) const {
    const auto it = routes_.find(route_id);
    if (it == routes_.end()) {
        return nullptr;
    }
    return &it->second;
}

const RouteEntry* RouteTable::find_by_name(std::string_view route_name) const {
    const auto it = route_names_.find(std::string(route_name));
    if (it == route_names_.end()) {
        return nullptr;
    }
    return find(it->second);
}

const RouteEntry* RouteTable::only() const {
    if (routes_.size() != 1) {
        return nullptr;
    }
    return &routes_.begin()->second;
}

bool RouteTable::contains(std::uint32_t route_id) const {
    return routes_.contains(route_id);
}

void RouteTable::clear() {
    routes_.clear();
    route_names_.clear();
}

std::size_t RouteTable::size() const {
    return routes_.size();
}

Envelope::Envelope(EnvelopeConfig config) : config_(config) {}

void Envelope::reset() {
    buffer_.clear();
    error_.clear();
}

LenPrefixEnvelope::LenPrefixEnvelope(EnvelopeConfig config)
    : Envelope(config) {
    if (config_.length_bytes == 0) {
        config_.length_bytes = 4;
    }
    config_.route_id_bytes = 0;
    config_.route_source = RouteSource::Body;
}

std::vector<Packet> LenPrefixEnvelope::feed(const std::uint8_t* data,
                                            std::size_t size) {
    std::vector<Packet> packets;
    error_.clear();

    if (data == nullptr && size > 0) {
        error_ = "lenprefix feed received null data";
        buffer_.clear();
        return packets;
    }
    if (!valid_integer_width(config_.length_bytes)) {
        error_ = "invalid lenprefix length width";
        buffer_.clear();
        return packets;
    }

    if (size > 0) {
        buffer_.insert(buffer_.end(), data, data + size);
    }
    const auto header_size = static_cast<std::size_t>(config_.length_bytes);

    while (buffer_.size() >= header_size) {
        const auto length_field =
            read_uint(ByteSpan(buffer_.data(), header_size), config_.endian);
        const auto maybe_payload_len = body_length_from_field(
            length_field, header_size, config_.length_includes_header);
        if (!maybe_payload_len) {
            error_ = "lenprefix frame length overflow";
            buffer_.clear();
            break;
        }
        const auto payload_len = *maybe_payload_len;
        if (payload_len > std::numeric_limits<std::size_t>::max() -
                              header_size) {
            error_ = "lenprefix frame length overflow";
            buffer_.clear();
            break;
        }
        const auto total_len = payload_len + header_size;
        if (config_.max_frame_size > 0 && payload_len > config_.max_frame_size) {
            error_ = "lenprefix frame too large";
            buffer_.clear();
            break;
        }
        if (buffer_.size() < total_len) {
            break;
        }

        const auto payload_begin = buffer_.begin() + header_size;
        const auto payload_end = payload_begin + static_cast<std::ptrdiff_t>(payload_len);

        Packet packet;
        packet.body.assign(payload_begin, payload_end);
        packet.raw_frame.assign(buffer_.begin(), buffer_.begin() +
                                                   static_cast<std::ptrdiff_t>(total_len));
        packets.push_back(std::move(packet));

        buffer_.erase(buffer_.begin(), buffer_.begin() +
                                         static_cast<std::ptrdiff_t>(total_len));
    }

    return packets;
}

std::vector<std::uint8_t> LenPrefixEnvelope::encode(const PacketRef& packet) {
    error_.clear();
    std::vector<std::uint8_t> out;

    if (!valid_integer_width(config_.length_bytes)) {
        error_ = "invalid lenprefix length width";
        return out;
    }
    const auto length_field = config_.length_includes_header
                                  ? packet.body.size() + config_.length_bytes
                                  : packet.body.size();
    if (!fits_width(length_field, config_.length_bytes)) {
        error_ = "lenprefix payload length does not fit header";
        return out;
    }

    out.reserve(config_.length_bytes + packet.body.size());
    write_uint(length_field, config_.length_bytes, config_.endian, out);
    out.insert(out.end(), packet.body.begin(), packet.body.end());
    return out;
}

IdLenEnvelope::IdLenEnvelope(EnvelopeConfig config) : Envelope(config) {
    if (config_.route_id_bytes == 0) {
        config_.route_id_bytes = 2;
    }
    if (config_.length_bytes == 0) {
        config_.length_bytes = 2;
    }
    config_.route_source = RouteSource::Header;
}

std::vector<Packet> IdLenEnvelope::feed(const std::uint8_t* data,
                                        std::size_t size) {
    std::vector<Packet> packets;
    error_.clear();

    if (data == nullptr && size > 0) {
        error_ = "idlen feed received null data";
        buffer_.clear();
        return packets;
    }
    if (!valid_integer_width(config_.route_id_bytes) ||
        !valid_integer_width(config_.length_bytes)) {
        error_ = "invalid idlen integer width";
        buffer_.clear();
        return packets;
    }

    if (size > 0) {
        buffer_.insert(buffer_.end(), data, data + size);
    }
    const auto header_size = static_cast<std::size_t>(config_.route_id_bytes) +
                             static_cast<std::size_t>(config_.length_bytes);

    while (buffer_.size() >= header_size) {
        const auto route_id = read_uint(
            ByteSpan(buffer_.data(), config_.route_id_bytes), config_.endian);
        const auto length_field = read_uint(
            ByteSpan(buffer_.data() + config_.route_id_bytes,
                     config_.length_bytes),
            config_.endian);

        if (route_id > std::numeric_limits<std::uint32_t>::max()) {
            error_ = "idlen route id overflow";
            buffer_.clear();
            break;
        }

        const auto maybe_payload_len = body_length_from_field(
            length_field, header_size, config_.length_includes_header);
        if (!maybe_payload_len) {
            error_ = "idlen frame length overflow";
            buffer_.clear();
            break;
        }
        const auto payload_len = *maybe_payload_len;
        if (payload_len > std::numeric_limits<std::size_t>::max() -
                              header_size) {
            error_ = "idlen frame length overflow";
            buffer_.clear();
            break;
        }
        const auto total_len = payload_len + header_size;
        if (config_.max_frame_size > 0 && payload_len > config_.max_frame_size) {
            error_ = "idlen frame too large";
            buffer_.clear();
            break;
        }
        if (buffer_.size() < total_len) {
            break;
        }

        const auto payload_begin = buffer_.begin() + header_size;
        const auto payload_end = payload_begin + static_cast<std::ptrdiff_t>(payload_len);

        Packet packet;
        packet.route_id = static_cast<std::uint32_t>(route_id);
        packet.body.assign(payload_begin, payload_end);
        packet.raw_frame.assign(buffer_.begin(), buffer_.begin() +
                                                   static_cast<std::ptrdiff_t>(total_len));
        packets.push_back(std::move(packet));

        buffer_.erase(buffer_.begin(), buffer_.begin() +
                                         static_cast<std::ptrdiff_t>(total_len));
    }

    return packets;
}

std::vector<std::uint8_t> IdLenEnvelope::encode(const PacketRef& packet) {
    error_.clear();
    std::vector<std::uint8_t> out;

    if (!valid_integer_width(config_.route_id_bytes) ||
        !valid_integer_width(config_.length_bytes)) {
        error_ = "invalid idlen integer width";
        return out;
    }
    if (!fits_width(packet.route_id, config_.route_id_bytes)) {
        error_ = "idlen route id does not fit header";
        return out;
    }
    const auto length_field = config_.length_includes_header
                                  ? packet.body.size() +
                                        config_.route_id_bytes +
                                        config_.length_bytes
                                  : packet.body.size();
    if (!fits_width(length_field, config_.length_bytes)) {
        error_ = "idlen payload length does not fit header";
        return out;
    }

    out.reserve(config_.route_id_bytes + config_.length_bytes +
                packet.body.size());
    write_uint(packet.route_id, config_.route_id_bytes, config_.endian, out);
    write_uint(length_field, config_.length_bytes, config_.endian, out);
    out.insert(out.end(), packet.body.begin(), packet.body.end());
    return out;
}

TypeLenEnvelope::TypeLenEnvelope(EnvelopeConfig config) : Envelope(config) {
    if (config_.route_id_bytes == 0) {
        config_.route_id_bytes = 1;
    }
    if (config_.length_bytes == 0) {
        config_.length_bytes = 4;
    }
    config_.route_source = RouteSource::Header;
}

std::vector<Packet> TypeLenEnvelope::feed(const std::uint8_t* data,
                                          std::size_t size) {
    std::vector<Packet> packets;
    error_.clear();

    if (data == nullptr && size > 0) {
        error_ = "typed_len feed received null data";
        buffer_.clear();
        return packets;
    }
    if (!valid_integer_width(config_.route_id_bytes) ||
        !valid_integer_width(config_.length_bytes)) {
        error_ = "invalid typed_len integer width";
        buffer_.clear();
        return packets;
    }

    if (size > 0) {
        buffer_.insert(buffer_.end(), data, data + size);
    }
    const auto header_size = static_cast<std::size_t>(config_.route_id_bytes) +
                             static_cast<std::size_t>(config_.length_bytes);

    while (buffer_.size() >= header_size) {
        const auto type_id = read_uint(
            ByteSpan(buffer_.data(), config_.route_id_bytes), config_.endian);
        const auto length_field = read_uint(
            ByteSpan(buffer_.data() + config_.route_id_bytes,
                     config_.length_bytes),
            config_.endian);

        if (type_id > std::numeric_limits<std::uint32_t>::max()) {
            error_ = "typed_len type id overflow";
            buffer_.clear();
            break;
        }

        const auto maybe_payload_len = body_length_from_field(
            length_field, header_size, config_.length_includes_header);
        if (!maybe_payload_len) {
            error_ = "typed_len frame length overflow";
            buffer_.clear();
            break;
        }
        const auto payload_len = *maybe_payload_len;
        if (payload_len > std::numeric_limits<std::size_t>::max() -
                              header_size) {
            error_ = "typed_len frame length overflow";
            buffer_.clear();
            break;
        }
        const auto total_len = payload_len + header_size;
        if (config_.max_frame_size > 0 && payload_len > config_.max_frame_size) {
            error_ = "typed_len frame too large";
            buffer_.clear();
            break;
        }
        if (buffer_.size() < total_len) {
            break;
        }

        const auto payload_begin = buffer_.begin() + header_size;
        const auto payload_end =
            payload_begin + static_cast<std::ptrdiff_t>(payload_len);

        Packet packet;
        packet.route_id = static_cast<std::uint32_t>(type_id);
        packet.kind = static_cast<std::uint16_t>(
            type_id & std::numeric_limits<std::uint16_t>::max());
        packet.body.assign(payload_begin, payload_end);
        packet.raw_frame.assign(buffer_.begin(), buffer_.begin() +
                                                   static_cast<std::ptrdiff_t>(total_len));
        packets.push_back(std::move(packet));

        buffer_.erase(buffer_.begin(), buffer_.begin() +
                                         static_cast<std::ptrdiff_t>(total_len));
    }

    return packets;
}

std::vector<std::uint8_t> TypeLenEnvelope::encode(const PacketRef& packet) {
    error_.clear();
    std::vector<std::uint8_t> out;

    const auto type_id = packet.route_id != 0 ? packet.route_id : packet.kind;

    if (!valid_integer_width(config_.route_id_bytes) ||
        !valid_integer_width(config_.length_bytes)) {
        error_ = "invalid typed_len integer width";
        return out;
    }
    if (!fits_width(type_id, config_.route_id_bytes)) {
        error_ = "typed_len type id does not fit header";
        return out;
    }
    const auto length_field = config_.length_includes_header
                                  ? packet.body.size() +
                                        config_.route_id_bytes +
                                        config_.length_bytes
                                  : packet.body.size();
    if (!fits_width(length_field, config_.length_bytes)) {
        error_ = "typed_len payload length does not fit header";
        return out;
    }

    out.reserve(config_.route_id_bytes + config_.length_bytes +
                packet.body.size());
    write_uint(type_id, config_.route_id_bytes, config_.endian, out);
    write_uint(length_field, config_.length_bytes, config_.endian, out);
    out.insert(out.end(), packet.body.begin(), packet.body.end());
    return out;
}

DelimiterEnvelope::DelimiterEnvelope(EnvelopeConfig config)
    : Envelope(config) {
    config_.length_bytes = 0;
    config_.route_id_bytes = 0;
    config_.route_source = RouteSource::Body;
}

std::vector<Packet> DelimiterEnvelope::feed(const std::uint8_t* data,
                                            std::size_t size) {
    std::vector<Packet> packets;
    error_.clear();

    if (data == nullptr && size > 0) {
        error_ = "delimiter feed received null data";
        buffer_.clear();
        return packets;
    }

    if (size > 0) {
        buffer_.insert(buffer_.end(), data, data + size);
    }

    while (true) {
        const auto delimiter = static_cast<std::uint8_t>(config_.delimiter);
        const auto it = std::find(buffer_.begin(), buffer_.end(), delimiter);
        if (it == buffer_.end()) {
            if (config_.max_frame_size > 0 &&
                buffer_.size() > config_.max_frame_size) {
                error_ = "delimiter frame too large";
                buffer_.clear();
            }
            break;
        }

        const auto body_len =
            static_cast<std::size_t>(std::distance(buffer_.begin(), it));
        if (config_.max_frame_size > 0 && body_len > config_.max_frame_size) {
            error_ = "delimiter frame too large";
            buffer_.clear();
            break;
        }

        Packet packet;
        packet.body.assign(buffer_.begin(), it);
        packet.raw_frame.assign(buffer_.begin(), it + 1);
        packets.push_back(std::move(packet));

        buffer_.erase(buffer_.begin(), it + 1);
    }

    return packets;
}

std::vector<std::uint8_t> DelimiterEnvelope::encode(const PacketRef& packet) {
    error_.clear();
    std::vector<std::uint8_t> out;
    out.reserve(packet.body.size() + 1);
    out.insert(out.end(), packet.body.begin(), packet.body.end());
    out.push_back(static_cast<std::uint8_t>(config_.delimiter));
    return out;
}

std::unique_ptr<Envelope> create_envelope(EnvelopeKind kind,
                                          EnvelopeConfig config) {
    switch (kind) {
        case EnvelopeKind::LenPrefix:
            return std::make_unique<LenPrefixEnvelope>(config);
        case EnvelopeKind::IdLen:
            return std::make_unique<IdLenEnvelope>(config);
        case EnvelopeKind::TypeLen:
            return std::make_unique<TypeLenEnvelope>(config);
        case EnvelopeKind::Delimiter:
            return std::make_unique<DelimiterEnvelope>(config);
    }
    return nullptr;
}

std::unique_ptr<Envelope> create_envelope(std::string_view kind,
                                          EnvelopeConfig config) {
    if (kind == "lenprefix" || kind == "len-prefix" ||
        kind == "len_prefix") {
        return create_envelope(EnvelopeKind::LenPrefix, config);
    }
    if (kind == "idlen" || kind == "id-len" || kind == "id_len") {
        return create_envelope(EnvelopeKind::IdLen, config);
    }
    if (kind == "typed_len" || kind == "type_len" ||
        kind == "typed-len" || kind == "typelen") {
        return create_envelope(EnvelopeKind::TypeLen, config);
    }
    if (kind == "delimiter" || kind == "line") {
        return create_envelope(EnvelopeKind::Delimiter, config);
    }
    return nullptr;
}

std::optional<BodyRouteKey> BodyCodec::route_key(PacketRef) {
    return std::nullopt;
}

DecodedBody RawBodyCodec::decode(PacketRef packet, const RouteEntry& route) {
    DecodedBody body;
    body.route_id = route.route_id;
    body.codec_id = route.codec_id;
    body.schema_id = route.schema_id;
    body.bytes.assign(packet.body.begin(), packet.body.end());
    return body;
}

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
        const auto json = nlohmann::json::parse(packet.body.begin(),
                                                packet.body.end());
        return structured_route_key(json);
    } catch (...) {
        return std::nullopt;
    }
}

DecodedBody JsonBodyCodec::decode(PacketRef packet, const RouteEntry& route) {
    auto json =
        nlohmann::json::parse(packet.body.begin(), packet.body.end());
    return decode_structured_body(packet, route, std::move(json));
}

std::vector<std::uint8_t> JsonBodyCodec::encode(const DecodedBody& body,
                                                const RouteEntry& route,
                                                const ProtocolProfile&) {
    const auto json = encode_structured_message(body, route);
    const auto serialized = json.dump();
    return std::vector<std::uint8_t>(serialized.begin(), serialized.end());
}

std::optional<BodyRouteKey> MsgpackBodyCodec::route_key(PacketRef packet) {
    try {
        const auto message = nlohmann::json::from_msgpack(packet.body.begin(),
                                                          packet.body.end());
        return structured_route_key(message);
    } catch (...) {
        return std::nullopt;
    }
}

DecodedBody MsgpackBodyCodec::decode(PacketRef packet, const RouteEntry& route) {
    const auto message = nlohmann::json::from_msgpack(packet.body.begin(),
                                                      packet.body.end());
    return decode_structured_body(packet, route, message);
}

std::vector<std::uint8_t> MsgpackBodyCodec::encode(const DecodedBody& body,
                                                   const RouteEntry& route,
                                                   const ProtocolProfile&) {
    return nlohmann::json::to_msgpack(encode_structured_message(body, route));
}

std::unique_ptr<BodyCodec> create_body_codec(std::string_view name) {
    if (name == "raw") {
        return std::make_unique<RawBodyCodec>();
    }
    if (name == "json") {
        return std::make_unique<JsonBodyCodec>();
    }
    if (name == "msgpack") {
        return std::make_unique<MsgpackBodyCodec>();
    }
    if (name == "protobuf" || name == "fbs" || name == "flatbuffers" ||
        name == "sproto") {
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
    args.route_id = route.route_id;
    args.codec_id = route.codec_id;
    args.schema_id = route.schema_id;
    args.route_name = route.debug_name.empty() ? nullptr : route.debug_name.c_str();
    args.payload = packet.body.data();
    args.payload_size = packet.body.size();

    shield_protocol_decode_result_v1 out{};
    shield_error_v1 err{};
    const int rc = codec_->decode(codec_, &args, &out, &err);
    if (rc != 0) {
        throw std::runtime_error(plugin_error_message(err, "protocol decode failed"));
    }

    try {
        DecodedBody body;
        body.route_id = route.route_id;
        body.codec_id = route.codec_id;
        body.schema_id = route.schema_id;
        body.route_name = route.debug_name;
        body.bytes.assign(packet.body.begin(), packet.body.end());

        if (out.message_json != nullptr && out.message_json_size > 0) {
            const auto size =
                static_cast<std::size_t>(out.message_json_size);
            body.message = nlohmann::json::parse(
                out.message_json, out.message_json + size);
        } else {
            body.message = nlohmann::json::object();
        }

        if (codec_->free_decode_result) {
            codec_->free_decode_result(codec_, &out);
        }
        return body;
    } catch (...) {
        if (codec_->free_decode_result) {
            codec_->free_decode_result(codec_, &out);
        }
        throw;
    }
}

std::vector<std::uint8_t> ExternalBodyCodec::encode(
    const DecodedBody& body, const RouteEntry& route,
    const ProtocolProfile&) {
    if (codec_ == nullptr || codec_->encode == nullptr) {
        throw std::runtime_error("protocol codec provider '" + provider_ +
                                 "' is not available");
    }

    const auto message = plugin_business_message(body);
    const auto message_text = message.dump();

    shield_protocol_encode_args_v1 args{};
    args.route_id = route.route_id;
    args.codec_id = route.codec_id;
    args.schema_id = route.schema_id;
    args.route_name = route.debug_name.empty() ? nullptr : route.debug_name.c_str();
    args.message_json = message_text.data();
    args.message_json_size = message_text.size();

    shield_protocol_encode_result_v1 out{};
    shield_error_v1 err{};
    const int rc = codec_->encode(codec_, &args, &out, &err);
    if (rc != 0) {
        throw std::runtime_error(plugin_error_message(err, "protocol encode failed"));
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

bool load_xmldef_routes_from_string(std::string_view xml, RouteTable& routes,
                                    const XmldefCatalogOptions& options,
                                    std::string* error) {
    std::size_t pos = 0;

    while (true) {
        const auto open = xml.find('<', pos);
        if (open == std::string_view::npos) {
            break;
        }
        const auto close = xml.find('>', open + 1);
        if (close == std::string_view::npos) {
            if (error) *error = "xmldef contains an unterminated tag";
            return false;
        }

        const auto raw_tag = xml.substr(open + 1, close - open - 1);
        pos = close + 1;
        const auto tag_text = trim_copy(raw_tag);
        std::string_view tag(tag_text);
        if (tag.empty() || tag.front() == '/' || tag.front() == '?' ||
            tag.front() == '!') {
            continue;
        }

        const auto name_end = tag.find_first_of(" \t\r\n/");
        const auto tag_name = tag.substr(0, name_end);
        if (tag_name != "message" && tag_name != "route") {
            continue;
        }

        const auto attr_text = name_end == std::string_view::npos
                                   ? std::string_view{}
                                   : tag.substr(name_end + 1);
        const auto attrs = parse_xml_attributes(attr_text);
        const auto* id_attr = find_attr(attrs, "id");
        if (id_attr == nullptr) {
            if (error) *error = "xmldef message/route is missing id";
            return false;
        }

        const auto route_id = parse_u32(*id_attr);
        if (!route_id || *route_id == 0) {
            if (error) *error = "xmldef message/route id must be uint32";
            return false;
        }

        RouteEntry entry;
        entry.route_id = *route_id;
        entry.codec_id = options.default_codec_id;
        entry.policy.action = options.default_action;
        entry.policy.lazy_decode = options.default_lazy_decode;

        if (const auto* value = find_attr(attrs, "name")) {
            entry.debug_name = *value;
        }
        if (const auto* value = find_attr(attrs, "target_service")) {
            const auto parsed = parse_u32(*value);
            if (!parsed) {
                if (error) {
                    *error =
                        "xmldef target_service must be uint32 when present";
                }
                return false;
            }
            entry.target_service = *parsed;
        } else if (const auto* value = find_attr(attrs, "target")) {
            const auto parsed = parse_u32(*value);
            if (!parsed) {
                if (error) *error = "xmldef target must be uint32 when present";
                return false;
            }
            entry.target_service = *parsed;
        }
        if (const auto* value = find_attr(attrs, "codec_id")) {
            const auto parsed = parse_u16(*value);
            if (!parsed) {
                if (error) *error = "xmldef codec_id must be uint16";
                return false;
            }
            entry.codec_id = *parsed;
        }
        if (const auto* value = find_attr(attrs, "schema_id")) {
            const auto parsed = parse_u16(*value);
            if (!parsed) {
                if (error) *error = "xmldef schema_id must be uint16";
                return false;
            }
            entry.schema_id = *parsed;
        } else if (const auto* value = find_attr(attrs, "schema")) {
            const auto parsed = parse_u16(*value);
            if (!parsed) {
                if (error) *error = "xmldef schema must be uint16";
                return false;
            }
            entry.schema_id = *parsed;
        }
        if (const auto* value = find_attr(attrs, "action")) {
            const auto parsed = parse_action(*value);
            if (!parsed) {
                if (error) *error = "xmldef action is invalid";
                return false;
            }
            entry.policy.action = *parsed;
        }
        if (const auto* value = find_attr(attrs, "lazy_decode")) {
            const auto parsed = parse_bool(*value);
            if (!parsed) {
                if (error) *error = "xmldef lazy_decode must be bool";
                return false;
            }
            entry.policy.lazy_decode = *parsed;
        }

        if (!routes.add(std::move(entry))) {
            if (error) *error = "xmldef contains duplicate route id or name";
            return false;
        }
    }

    return true;
}

bool load_xmldef_routes_from_file(std::string_view path, RouteTable& routes,
                                  const XmldefCatalogOptions& options,
                                  std::string* error) {
    std::ifstream file{std::filesystem::path(std::string(path))};
    if (!file.is_open()) {
        if (error) *error = "failed to open xmldef catalog";
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_xmldef_routes_from_string(buffer.str(), routes, options, error);
}

bool BodyCodecRegistry::add(std::uint16_t codec_id,
                            std::unique_ptr<BodyCodec> codec) {
    if (!codec || codecs_.contains(codec_id)) {
        return false;
    }

    const auto codec_name = std::string(codec->name());
    if (!codec_name.empty() && codec_names_.contains(codec_name)) {
        return false;
    }

    codecs_.emplace(codec_id, std::move(codec));
    if (!codec_name.empty()) {
        codec_names_.emplace(codec_name, codec_id);
    }
    return true;
}

void BodyCodecRegistry::upsert(std::uint16_t codec_id,
                               std::unique_ptr<BodyCodec> codec) {
    if (!codec) {
        return;
    }

    const auto existing = codecs_.find(codec_id);
    if (existing != codecs_.end()) {
        const auto old_name = std::string(existing->second->name());
        const auto old_name_it = codec_names_.find(old_name);
        if (old_name_it != codec_names_.end() &&
            old_name_it->second == codec_id) {
            codec_names_.erase(old_name_it);
        }
    }

    const auto codec_name = std::string(codec->name());
    codecs_.insert_or_assign(codec_id, std::move(codec));
    if (!codec_name.empty()) {
        codec_names_.insert_or_assign(codec_name, codec_id);
    }
}

BodyCodec* BodyCodecRegistry::find(std::uint16_t codec_id) {
    const auto it = codecs_.find(codec_id);
    if (it == codecs_.end()) {
        return nullptr;
    }
    return it->second.get();
}

const BodyCodec* BodyCodecRegistry::find(std::uint16_t codec_id) const {
    const auto it = codecs_.find(codec_id);
    if (it == codecs_.end()) {
        return nullptr;
    }
    return it->second.get();
}

BodyCodec* BodyCodecRegistry::find_by_name(std::string_view name) {
    const auto it = codec_names_.find(std::string(name));
    if (it == codec_names_.end()) {
        return nullptr;
    }
    return find(it->second);
}

const BodyCodec* BodyCodecRegistry::find_by_name(std::string_view name) const {
    const auto it = codec_names_.find(std::string(name));
    if (it == codec_names_.end()) {
        return nullptr;
    }
    return find(it->second);
}

void BodyCodecRegistry::clear() {
    codecs_.clear();
    codec_names_.clear();
}

std::size_t BodyCodecRegistry::size() const {
    return codecs_.size();
}

ProtocolPipeline::ProtocolPipeline(ProtocolProfile profile, RouteTable routes,
                                   BodyCodecRegistry codecs)
    : profile_(std::move(profile)),
      envelope_(create_envelope(profile_.envelope_kind, profile_.envelope)),
      routes_(std::move(routes)),
      codecs_(std::move(codecs)) {
    if (!envelope_) {
        error_ = "failed to create protocol envelope";
    }
}

std::vector<DispatchResult> ProtocolPipeline::feed(const std::uint8_t* data,
                                                   std::size_t size) {
    std::vector<DispatchResult> results;
    error_.clear();

    if (!envelope_) {
        error_ = "protocol envelope is not configured";
        results.push_back(DispatchResult{.error = error_});
        return results;
    }

    auto packets = envelope_->feed(data, size);
    if (envelope_->has_error()) {
        error_ = envelope_->error();
        results.push_back(DispatchResult{.error = error_});
        return results;
    }

    results.reserve(packets.size());
    for (auto& packet : packets) {
        DispatchResult result;
        result.packet = std::move(packet);

        const auto* route = resolve_route(result.packet, result);
        result.route = route;
        if (!result.ok()) {
            results.push_back(std::move(result));
            continue;
        }

        if (route == nullptr) {
            result.action = profile_.unknown_route_action;
            results.push_back(std::move(result));
            continue;
        }

        result.action = route->policy.action;
        if (result.action == RouteAction::Drop) {
            results.push_back(std::move(result));
            continue;
        }
        const auto should_decode = result.action == RouteAction::DecodeLocal &&
                                   (profile_.decode_before_dispatch ||
                                    !route->policy.lazy_decode);
        if (should_decode) {
            auto* codec = codec_for_route(*route);
            if (codec == nullptr) {
                result.error = "body codec is not registered";
                results.push_back(std::move(result));
                continue;
            }
            try {
                result.decoded_body = codec->decode(result.packet.ref(), *route);
            } catch (const std::exception& ex) {
                result.error = std::string("body decode failed: ") + ex.what();
            }
        }

        results.push_back(std::move(result));
    }

    return results;
}

std::vector<std::uint8_t> ProtocolPipeline::encode(const PacketRef& packet) {
    error_.clear();
    if (!envelope_) {
        error_ = "protocol envelope is not configured";
        return {};
    }

    auto encoded = envelope_->encode(packet);
    if (envelope_->has_error()) {
        error_ = envelope_->error();
    }
    return encoded;
}

std::vector<std::uint8_t> ProtocolPipeline::encode_message(DecodedBody body) {
    error_.clear();
    if (!envelope_) {
        error_ = "protocol envelope is not configured";
        return {};
    }

    const auto* route = resolve_outbound_route(body);
    if (route == nullptr) {
        error_ = "failed to resolve outbound route";
        return {};
    }

    auto* codec = codec_for_route(*route);
    if (codec == nullptr) {
        error_ = "body codec is not registered";
        return {};
    }

    std::vector<std::uint8_t> body_bytes;
    try {
        body_bytes = codec->encode(body, *route, profile_);
    } catch (const std::exception& ex) {
        error_ = std::string("body encode failed: ") + ex.what();
        return {};
    }

    Packet packet;
    packet.route_id = route->route_id;
    packet.kind = static_cast<std::uint16_t>(route->kind);
    packet.body = std::move(body_bytes);
    return encode(packet.ref());
}

bool ProtocolPipeline::materialize_decode(DispatchResult& result) {
    if (!result.ok() || result.action != RouteAction::DecodeLocal) {
        return result.ok();
    }
    if (result.decoded()) {
        return true;
    }
    if (result.route == nullptr) {
        result.error = "decode_local dispatch is missing route metadata";
        return false;
    }

    auto* codec = codec_for_route(*result.route);
    if (codec == nullptr) {
        result.error = "body codec is not registered";
        return false;
    }

    try {
        result.decoded_body = codec->decode(result.packet.ref(), *result.route);
        return true;
    } catch (const std::exception& ex) {
        result.error = std::string("body decode failed: ") + ex.what();
        return false;
    }
}

void ProtocolPipeline::reset() {
    error_.clear();
    if (envelope_) {
        envelope_->reset();
    }
}

std::string_view ProtocolPipeline::default_codec_name() const {
    const auto* codec = codecs_.find(profile_.default_codec_id);
    if (codec == nullptr) {
        return {};
    }
    return codec->name();
}

const RouteEntry* ProtocolPipeline::resolve_route(Packet& packet,
                                                  DispatchResult& result) {
    if (packet.route_id != 0) {
        if (const auto* route = routes_.find(packet.route_id)) {
            return route;
        }
    }

    if (profile_.route_source != RouteSource::Body ||
        !profile_.decode_body_route) {
        return nullptr;
    }

    auto* codec = codecs_.find(profile_.default_codec_id);
    if (codec == nullptr) {
        result.error = "default body codec is not registered";
        return nullptr;
    }

    const auto key = codec->route_key(packet.ref());
    if (!key || key->empty()) {
        return nullptr;
    }

    if (key->route_id != 0) {
        packet.route_id = key->route_id;
        return routes_.find(key->route_id);
    }

    if (!key->route_name.empty()) {
        if (const auto* route = routes_.find_by_name(key->route_name)) {
            packet.route_id = route->route_id;
            return route;
        }
    }

    return nullptr;
}

BodyCodec* ProtocolPipeline::codec_for_route(const RouteEntry& route) {
    // Phase 1: a pipeline binds a single body codec (profile_.default_codec_id).
    // route.codec_id is kept as route metadata / future extension slot and is
    // intentionally not used to select a per-route codec here.
    (void)route;
    return codecs_.find(profile_.default_codec_id);
}

const RouteEntry* ProtocolPipeline::resolve_outbound_route(DecodedBody& body) {
    bool saw_route_hint = body.route_id != 0 || !body.route_name.empty();

    if (body.route_id != 0) {
        if (const auto* route = routes_.find(body.route_id)) {
            if (body.route_name.empty()) {
                body.route_name = route->debug_name;
            }
            return route;
        }
    }

    if (!body.route_name.empty()) {
        if (const auto* route = routes_.find_by_name(body.route_name)) {
            body.route_id = route->route_id;
            return route;
        }
    }

    if (body.has_message() && body.message->is_object()) {
        const auto& message = *body.message;
        if (message.contains("route_id")) {
            saw_route_hint = true;
        }
        if (message.contains("route_id") &&
            message["route_id"].is_number_unsigned()) {
            const auto route_id = message["route_id"].get<std::uint32_t>();
            if (const auto* route = routes_.find(route_id)) {
                body.route_id = route_id;
                if (body.route_name.empty()) {
                    body.route_name = route->debug_name;
                }
                return route;
            }
        }
        if (message.contains("msg_id")) {
            saw_route_hint = true;
        }
        if (message.contains("msg_id") &&
            message["msg_id"].is_number_unsigned()) {
            const auto route_id = message["msg_id"].get<std::uint32_t>();
            if (const auto* route = routes_.find(route_id)) {
                body.route_id = route_id;
                if (body.route_name.empty()) {
                    body.route_name = route->debug_name;
                }
                return route;
            }
        }
        if (message.contains("route")) {
            saw_route_hint = true;
        }
        if (message.contains("route") && message["route"].is_string()) {
            const auto route_name = message["route"].get<std::string>();
            if (const auto* route = routes_.find_by_name(route_name)) {
                body.route_id = route->route_id;
                body.route_name = route_name;
                return route;
            }
        }
        if (message.contains("method")) {
            saw_route_hint = true;
        }
        if (message.contains("method") && message["method"].is_string()) {
            const auto route_name = message["method"].get<std::string>();
            if (const auto* route = routes_.find_by_name(route_name)) {
                body.route_id = route->route_id;
                body.route_name = route_name;
                return route;
            }
        }
    }

    if (!saw_route_hint) {
        if (const auto* route = routes_.only()) {
            body.route_id = route->route_id;
            if (body.route_name.empty()) {
                body.route_name = route->debug_name;
            }
            return route;
        }
    }

    return nullptr;
}

std::unique_ptr<ProtocolPipeline> build_protocol_pipeline_from_json(
    std::string_view config_json, const ProtocolBuildOptions& options,
    std::string* error) {
    try {
        nlohmann::json config =
            nlohmann::json::parse(config_json, nullptr, false);
        if (config.is_discarded() || !config.is_object() || config.empty()) {
            return nullptr;
        }

        ProtocolProfile profile;
        profile.name = config.value("name", std::string{});

        const auto envelope = config.value("envelope", nlohmann::json::object());
        if (envelope.is_object()) {
            if (envelope.contains("type")) {
                const auto parsed =
                    parse_envelope_kind(envelope["type"].get<std::string>());
                if (!parsed) {
                    if (error) *error = "unknown network.protocol.envelope.type";
                    return nullptr;
                }
                profile.envelope_kind = *parsed;
            }
            if (envelope.contains("endian")) {
                const auto parsed = parse_protocol_endian(envelope["endian"]);
                if (!parsed) {
                    if (error) *error = "invalid network.protocol.envelope.endian";
                    return nullptr;
                }
                profile.envelope.endian = *parsed;
            }
            profile.envelope.length_bytes =
                static_cast<std::uint8_t>(envelope.value("length_bytes", 0));
            profile.envelope.route_id_bytes =
                static_cast<std::uint8_t>(envelope.value("route_id_bytes", 0));
            profile.envelope.length_includes_header =
                envelope.value("length_includes_header", false);
            if (envelope.contains("delimiter") &&
                envelope["delimiter"].is_string()) {
                const auto delimiter = envelope["delimiter"].get<std::string>();
                if (!delimiter.empty()) {
                    profile.envelope.delimiter =
                        static_cast<std::uint8_t>(delimiter.front());
                }
            }
            profile.envelope.max_frame_size =
                envelope.value("max_frame_size", std::size_t{0});
        }
        if (profile.envelope.max_frame_size == 0 &&
            options.fallback_max_frame_size > 0) {
            profile.envelope.max_frame_size = options.fallback_max_frame_size;
        }
        profile.route_source =
            default_route_source_for_envelope(profile.envelope_kind);

        const auto body = config.value("body", nlohmann::json::object());
        const std::string body_codec = body.value("codec", "raw");
        const std::string body_provider = body.value("provider", "");

        BodyCodecRegistry codecs;
        constexpr std::uint16_t default_codec_id = 1;
        std::unique_ptr<BodyCodec> codec;
        if (!body_provider.empty()) {
            if (!options.external_codec_resolver) {
                if (error) {
                    *error = "network.protocol.body.provider is configured "
                             "but no external codec resolver is available";
                }
                return nullptr;
            }
            std::string resolver_error;
            const auto* external = options.external_codec_resolver(
                body_provider, body_codec, &resolver_error);
            if (external == nullptr) {
                if (error) {
                    *error = resolver_error.empty()
                                 ? "failed to resolve network.protocol.body.provider"
                                 : resolver_error;
                }
                return nullptr;
            }
            if (external->codec_name == nullptr ||
                std::string_view(external->codec_name) != body_codec) {
                if (error) {
                    *error = "network.protocol.body.provider does not serve "
                             "network.protocol.body.codec";
                }
                return nullptr;
            }
            if (external->decode == nullptr || external->encode == nullptr) {
                if (error) {
                    *error =
                        "network.protocol.body.provider has incomplete vtable";
                }
                return nullptr;
            }
            codec = std::make_unique<ExternalBodyCodec>(
                body_provider, body_codec, external);
        } else {
            codec = create_body_codec(body_codec);
        }
        if (!codec) {
            if (error) *error = "unsupported network.protocol.body.codec";
            return nullptr;
        }
        const std::string normalized_body_codec(codec->name());
        codecs.add(default_codec_id, std::move(codec));
        profile.default_codec_id = default_codec_id;

        const auto routing = config.value("routing", nlohmann::json::object());
        if (routing.is_object()) {
            if (routing.contains("source") && routing["source"].is_string()) {
                const auto parsed =
                    parse_route_source(routing["source"].get<std::string>());
                if (!parsed) {
                    if (error) *error = "invalid network.protocol.routing.source";
                    return nullptr;
                }
                profile.route_source = *parsed;
            }
            profile.decode_body_route = routing.value("decode_body_route", true);
            profile.decode_before_dispatch =
                routing.value("decode_before_dispatch", false);
            if (routing.contains("unknown_route_action") &&
                routing["unknown_route_action"].is_string()) {
                const auto parsed =
                    parse_action(routing["unknown_route_action"].get<std::string>());
                if (!parsed) {
                    if (error) {
                        *error =
                            "invalid network.protocol.routing.unknown_route_action";
                    }
                    return nullptr;
                }
                profile.unknown_route_action = *parsed;
            }
        }

        RouteTable routes;
        if (normalized_body_codec == "xmldef" && body.contains("catalog") &&
            body["catalog"].is_string()) {
            XmldefCatalogOptions catalog_options;
            catalog_options.default_codec_id = default_codec_id;
            if (routing.contains("default_action") &&
                routing["default_action"].is_string()) {
                const auto parsed =
                    parse_action(routing["default_action"].get<std::string>());
                if (!parsed) {
                    if (error) {
                        *error = "invalid network.protocol.routing.default_action";
                    }
                    return nullptr;
                }
                catalog_options.default_action = *parsed;
            }
            catalog_options.default_lazy_decode =
                routing.value("lazy_decode", true);

            std::filesystem::path catalog_path(body["catalog"].get<std::string>());
            if (catalog_path.is_relative() && !options.source_dir.empty()) {
                catalog_path = std::filesystem::path(std::string(options.source_dir)) /
                               catalog_path;
            }

            std::string catalog_error;
            if (!load_xmldef_routes_from_file(catalog_path.string(), routes,
                                              catalog_options, &catalog_error)) {
                if (error) {
                    *error = "failed to load xmldef catalog: " + catalog_error;
                }
                return nullptr;
            }
        }

        if (config.contains("routes") && config["routes"].is_array()) {
            for (const auto& route : config["routes"]) {
                if (!route.is_object()) {
                    continue;
                }
                RouteEntry entry;
                entry.route_id = route.value("id", std::uint32_t{0});
                entry.target_service =
                    route.value("target_service", std::uint32_t{0});
                entry.codec_id = route.value("codec_id", default_codec_id);
                entry.schema_id = route.value("schema_id", std::uint16_t{0});
                entry.debug_name = route.value("name", std::string{});
                entry.policy.lazy_decode = route.value("lazy_decode", true);
                if (route.contains("action") && route["action"].is_string()) {
                    const auto parsed =
                        parse_action(route["action"].get<std::string>());
                    if (!parsed) {
                        if (error) {
                            *error = "invalid network.protocol.routes.action";
                        }
                        return nullptr;
                    }
                    entry.policy.action = *parsed;
                }
                if (entry.route_id == 0) {
                    if (error) *error = "network.protocol.routes[].id is required";
                    return nullptr;
                }

                if (!entry.debug_name.empty()) {
                    if (const auto* named = routes.find_by_name(entry.debug_name);
                        named != nullptr && named->route_id != entry.route_id) {
                        if (error) {
                            *error =
                                "network.protocol.routes contains duplicate name";
                        }
                        return nullptr;
                    }
                }

                routes.upsert(std::move(entry));
            }
        }

        return std::make_unique<ProtocolPipeline>(std::move(profile),
                                                  std::move(routes),
                                                  std::move(codecs));
    } catch (const nlohmann::json::exception& ex) {
        if (error) {
            *error = std::string("invalid network.protocol: ") + ex.what();
        }
        return nullptr;
    }
}

std::unique_ptr<ProtocolPipeline> build_protocol_pipeline_from_json(
    std::string_view config_json, std::string_view source_dir,
    std::size_t fallback_max_frame_size, std::string* error) {
    ProtocolBuildOptions options;
    options.source_dir = source_dir;
    options.fallback_max_frame_size = fallback_max_frame_size;
    return build_protocol_pipeline_from_json(config_json, options, error);
}

}  // namespace shield::transport
