// [SHIELD_TRANSPORT] Frame envelopes: the length/id+length/type+length
// delimiter framings with their byte-level feed/encode helpers, plus
// create_envelope.
// Split out of protocol.cpp.
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
#include "shield/transport/protocol.hpp"
#include "shield/transport/rpc_descriptor.hpp"

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
        if (length_field >  // GCOVR_EXCL_BR_LINE (unreachable on
                            // 64-bit, see below)
            std::numeric_limits<std::size_t>::max()) {  // (unreachable on
                                                        // 64-bit)
            return std::nullopt;  // GCOVR_EXCL_LINE (unreachable on 64-bit)
        }
        return static_cast<std::size_t>(length_field);
    }

    if (length_field < header_size ||  // GCOVR_EXCL_BR_LINE (unreachable on
                                       // 64-bit: the "> size_t max" arm)
        length_field > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(length_field) - header_size;
}
}  // namespace

Envelope::Envelope(EnvelopeConfig config) : config_(config) {}

void Envelope::reset() {
    buffer_.clear();
    error_.clear();
}

LenPrefixEnvelope::LenPrefixEnvelope(EnvelopeConfig config) : Envelope(config) {
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
        if (payload_len >
            std::numeric_limits<std::size_t>::max() - header_size) {
            error_ = "lenprefix frame length overflow";
            buffer_.clear();
            break;
        }
        const auto total_len = payload_len + header_size;
        if (payload_len > effective_max_frame_size()) {
            error_ = "lenprefix frame too large";
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
        packet.body.assign(payload_begin, payload_end);
        packet.raw_frame.assign(
            buffer_.begin(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(total_len));
        packets.push_back(std::move(packet));

        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(total_len));
    }

    return packets;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

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
}  // GCOVR_EXCL_LINE (uncalled exit clone)

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
        const auto length_field =
            read_uint(ByteSpan(buffer_.data() + config_.route_id_bytes,
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
        if (payload_len >
            std::numeric_limits<std::size_t>::max() - header_size) {
            error_ = "idlen frame length overflow";
            buffer_.clear();
            break;
        }
        const auto total_len = payload_len + header_size;
        if (payload_len > effective_max_frame_size()) {
            error_ = "idlen frame too large";
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
        packet.route_id = static_cast<std::uint32_t>(route_id);
        packet.body.assign(payload_begin, payload_end);
        packet.raw_frame.assign(
            buffer_.begin(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(total_len));
        packets.push_back(std::move(packet));

        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(total_len));
    }

    return packets;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

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
    const auto length_field =
        config_.length_includes_header
            ? packet.body.size() + config_.route_id_bytes + config_.length_bytes
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
}  // GCOVR_EXCL_LINE (uncalled exit clone)

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
        const auto length_field =
            read_uint(ByteSpan(buffer_.data() + config_.route_id_bytes,
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
        if (payload_len >
            std::numeric_limits<std::size_t>::max() - header_size) {
            error_ = "typed_len frame length overflow";
            buffer_.clear();
            break;
        }
        const auto total_len = payload_len + header_size;
        if (payload_len > effective_max_frame_size()) {
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
        packet.raw_frame.assign(
            buffer_.begin(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(total_len));
        packets.push_back(std::move(packet));

        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(total_len));
    }

    return packets;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

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
    const auto length_field =
        config_.length_includes_header
            ? packet.body.size() + config_.route_id_bytes + config_.length_bytes
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
}  // GCOVR_EXCL_LINE (uncalled exit clone)

DelimiterEnvelope::DelimiterEnvelope(EnvelopeConfig config) : Envelope(config) {
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
            if (buffer_.size() > effective_max_frame_size()) {
                error_ = "delimiter frame too large";
                buffer_.clear();
            }
            break;
        }

        const auto body_len =
            static_cast<std::size_t>(std::distance(buffer_.begin(), it));
        if (body_len > effective_max_frame_size()) {
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
}  // GCOVR_EXCL_LINE (uncalled exit clone)

std::vector<std::uint8_t> DelimiterEnvelope::encode(const PacketRef& packet) {
    error_.clear();
    std::vector<std::uint8_t> out;
    out.reserve(packet.body.size() + 1);
    out.insert(out.end(), packet.body.begin(), packet.body.end());
    out.push_back(static_cast<std::uint8_t>(config_.delimiter));
    return out;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

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
    if (kind == "lenprefix" || kind == "len-prefix" || kind == "len_prefix") {
        return create_envelope(EnvelopeKind::LenPrefix, config);
    }
    if (kind == "idlen" || kind == "id-len" || kind == "id_len") {
        return create_envelope(EnvelopeKind::IdLen, config);
    }
    if (kind == "typed_len" || kind == "type_len" || kind == "typed-len" ||
        kind == "typelen") {
        return create_envelope(EnvelopeKind::TypeLen, config);
    }
    if (kind == "delimiter" || kind == "line") {
        return create_envelope(EnvelopeKind::Delimiter, config);
    }
    return nullptr;
}

}  // namespace shield::transport
