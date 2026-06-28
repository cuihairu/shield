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
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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
    body.codec_id = route.codec_id;
    body.schema_id = route.schema_id;
    body.bytes.assign(packet.body.begin(), packet.body.end());
    return body;
}

std::vector<std::uint8_t> RawBodyCodec::encode(const DecodedBody& body) {
    return body.bytes;
}

PassthroughBodyCodec::PassthroughBodyCodec(std::string name)
    : name_(std::move(name)) {}

DecodedBody PassthroughBodyCodec::decode(PacketRef packet,
                                         const RouteEntry& route) {
    DecodedBody body;
    body.codec_id = route.codec_id;
    body.schema_id = route.schema_id;
    body.bytes.assign(packet.body.begin(), packet.body.end());
    return body;
}

std::vector<std::uint8_t> PassthroughBodyCodec::encode(
    const DecodedBody& body) {
    return body.bytes;
}

std::optional<BodyRouteKey> JsonBodyCodec::route_key(PacketRef packet) {
    try {
        const auto json = nlohmann::json::parse(packet.body.begin(),
                                                packet.body.end());
        BodyRouteKey key;

        if (json.contains("route_id") && json["route_id"].is_number_unsigned()) {
            key.route_id = json["route_id"].get<std::uint32_t>();
        } else if (json.contains("msg_id") &&
                   json["msg_id"].is_number_unsigned()) {
            key.route_id = json["msg_id"].get<std::uint32_t>();
        }

        if (json.contains("route") && json["route"].is_string()) {
            key.route_name = json["route"].get<std::string>();
        } else if (json.contains("method") && json["method"].is_string()) {
            key.route_name = json["method"].get<std::string>();
        }

        if (key.empty()) {
            return std::nullopt;
        }
        return key;
    } catch (...) {
        return std::nullopt;
    }
}

DecodedBody JsonBodyCodec::decode(PacketRef packet, const RouteEntry& route) {
    DecodedBody body;
    body.codec_id = route.codec_id;
    body.schema_id = route.schema_id;
    body.bytes.assign(packet.body.begin(), packet.body.end());

    if (auto key = route_key(packet)) {
        body.route_name = std::move(key->route_name);
    }

    return body;
}

std::vector<std::uint8_t> JsonBodyCodec::encode(const DecodedBody& body) {
    return body.bytes;
}

std::unique_ptr<BodyCodec> create_body_codec(std::string_view name) {
    if (name == "raw") {
        return std::make_unique<RawBodyCodec>();
    }
    if (name == "json") {
        return std::make_unique<JsonBodyCodec>();
    }
    if (name == "msgpack" || name == "protobuf" || name == "fbs" ||
        name == "flatbuffers" || name == "sproto") {
        return std::make_unique<PassthroughBodyCodec>(std::string(name));
    }
    if (name == "xmldef" || name == "xml_def") {
        return std::make_unique<PassthroughBodyCodec>("xmldef");
    }
    return nullptr;
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
            if (result.action == RouteAction::Drop) {
                result.error = "unknown protocol route";
            }
            results.push_back(std::move(result));
            continue;
        }

        result.action = route->policy.action;
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
            result.decoded_body = codec->decode(result.packet.ref(), *route);
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

void ProtocolPipeline::reset() {
    error_.clear();
    if (envelope_) {
        envelope_->reset();
    }
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
    if (route.codec_id != 0) {
        return codecs_.find(route.codec_id);
    }
    return codecs_.find(profile_.default_codec_id);
}

}  // namespace shield::transport
