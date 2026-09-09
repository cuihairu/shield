#define BOOST_TEST_MODULE CovProtocol
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "shield/plugin/protocol_codec.h"
#include "shield/transport/protocol.hpp"

using shield::transport::BodyCodecRegistry;
using shield::transport::build_protocol_pipeline_from_json;
using shield::transport::create_body_codec;
using shield::transport::create_envelope;
using shield::transport::DecodedBody;
using shield::transport::DelimiterEnvelope;
using shield::transport::DispatchResult;
using shield::transport::Endian;
using shield::transport::EnvelopeConfig;
using shield::transport::EnvelopeKind;
using shield::transport::ExternalBodyCodec;
using shield::transport::IdLenEnvelope;
using shield::transport::JsonBodyCodec;
using shield::transport::LenPrefixEnvelope;
using shield::transport::load_xmldef_routes_from_file;
using shield::transport::load_xmldef_routes_from_string;
using shield::transport::Packet;
using shield::transport::PacketRef;
using shield::transport::PassthroughBodyCodec;
using shield::transport::ProtocolBuildOptions;
using shield::transport::ProtocolPipeline;
using shield::transport::ProtocolProfile;
using shield::transport::RawBodyCodec;
using shield::transport::RouteAction;
using shield::transport::RouteDirection;
using shield::transport::RouteEntry;
using shield::transport::RoutePolicy;
using shield::transport::RouteSource;
using shield::transport::RouteTable;
using shield::transport::TypeLenEnvelope;
using shield::transport::XmldefCatalogOptions;

namespace {

std::vector<std::uint8_t> bytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

std::vector<std::uint8_t> concat(std::vector<std::uint8_t> a,
                                 const std::vector<std::uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<std::uint8_t> be_bytes(std::uint64_t value, std::size_t width) {
    std::vector<std::uint8_t> out(width);
    for (std::size_t i = 0; i < width; ++i) {
        out[i] =
            static_cast<std::uint8_t>((value >> ((width - 1 - i) * 8)) & 0xff);
    }
    return out;
}

// route_id (2 bytes LE) + length (2 bytes LE) + body
std::vector<std::uint8_t> idlen_le_frame(std::uint16_t route_id,
                                         std::string_view body) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(route_id & 0xff));
    out.push_back(static_cast<std::uint8_t>((route_id >> 8) & 0xff));
    const auto size = static_cast<std::uint16_t>(body.size());
    out.push_back(static_cast<std::uint8_t>(size & 0xff));
    out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xff));
    const auto body_bytes = bytes(body);
    out.insert(out.end(), body_bytes.begin(), body_bytes.end());
    return out;
}

char* dup_cstr(std::string_view value) {
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

std::uint8_t* dup_bin(std::string_view value) {
    if (value.empty()) return nullptr;
    auto* out = static_cast<std::uint8_t*>(std::malloc(value.size()));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    return out;
}

struct FakeExtState {
    int decode_rc = 0;
    int encode_rc = 0;
    const char* err_code = "";
    const char* err_message = "";
    std::string decoded_json = R"({"uid":7})";
    bool null_message = false;
    bool free_encode_throws = false;
    bool free_encode_throw_once = false;
    std::string last_encode_input;
    std::string last_decode_route_name;
    std::uint32_t last_route_id = 0;
};

int fake_ext_decode(const shield_protocol_codec_v1* self,
                    const shield_protocol_decode_args_v1* args,
                    shield_protocol_decode_result_v1* out,
                    shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        return -1;
    }
    auto* state = static_cast<FakeExtState*>(self->user_data);
    state->last_route_id = args->route_id;
    state->last_decode_route_name = args->route_name ? args->route_name : "";
    if (state->decode_rc != 0) {
        if (err) {
            err->code = state->err_code;
            err->message = state->err_message;
            err->phase = "test";
        }
        return state->decode_rc;
    }
    if (!state->null_message) {
        out->message_json = dup_cstr(state->decoded_json);
        out->message_json_size = state->decoded_json.size();
        if (out->message_json == nullptr) return -1;
    }
    return 0;
}

void fake_ext_free_decode(const shield_protocol_codec_v1*,
                          shield_protocol_decode_result_v1* result) {
    if (result == nullptr) return;
    std::free(const_cast<char*>(result->message_json));
    result->message_json = nullptr;
    result->message_json_size = 0;
}

int fake_ext_encode(const shield_protocol_codec_v1* self,
                    const shield_protocol_encode_args_v1* args,
                    shield_protocol_encode_result_v1* out,
                    shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        return -1;
    }
    auto* state = static_cast<FakeExtState*>(self->user_data);
    state->last_route_id = args->route_id;
    if (state->encode_rc != 0) {
        if (err) {
            err->code = state->err_code;
            err->message = state->err_message;
            err->phase = "test";
        }
        return state->encode_rc;
    }
    if (args->message_json != nullptr) {
        state->last_encode_input.assign(
            args->message_json, args->message_json + args->message_json_size);
    } else {
        state->last_encode_input.clear();
    }
    out->payload = dup_bin("ENC");
    out->payload_size = 3;
    return out->payload == nullptr ? -1 : 0;
}

void fake_ext_free_encode(const shield_protocol_codec_v1* self,
                          shield_protocol_encode_result_v1* result) {
    auto* state = self ? static_cast<FakeExtState*>(self->user_data) : nullptr;
    if (state != nullptr && state->free_encode_throw_once) {
        state->free_encode_throw_once = false;
        throw std::runtime_error("free_encode_result failed once");
    }
    if (state != nullptr && state->free_encode_throws) {
        throw std::runtime_error("free_encode_result failed");
    }
    if (result == nullptr) return;
    std::free(const_cast<std::uint8_t*>(result->payload));
    result->payload = nullptr;
    result->payload_size = 0;
}

shield_protocol_codec_v1 make_ext_codec(FakeExtState& state,
                                        bool with_decode = true,
                                        bool with_encode = true) {
    shield_protocol_codec_v1 codec{};
    codec.struct_size = sizeof(shield_protocol_codec_v1);
    codec.codec_name = "protobuf";
    codec.version = "cov";
    codec.user_data = &state;
    codec.decode = with_decode ? fake_ext_decode : nullptr;
    codec.encode = with_encode ? fake_ext_encode : nullptr;
    codec.free_decode_result = fake_ext_free_decode;
    codec.free_encode_result = fake_ext_free_encode;
    return codec;
}

std::filesystem::path cov_temp_dir() {
    const auto dir =
        std::filesystem::temp_directory_path() / "shield_cov_protocol_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

// ---------------------------------------------------------------------------
// LenPrefixEnvelope
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovLenPrefixEnvelope)

BOOST_AUTO_TEST_CASE(Width8RoundTrip) {
    EnvelopeConfig config;
    config.length_bytes = 8;
    LenPrefixEnvelope envelope(config);

    Packet packet;
    packet.body = bytes("wide-body");
    const auto encoded = envelope.encode(packet.ref());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(encoded.size(), 8u + packet.body.size());

    auto packets = envelope.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(),
                                  packets[0].body.end(), packet.body.begin(),
                                  packet.body.end());
}

BOOST_AUTO_TEST_CASE(FeedNullDataReportsError) {
    LenPrefixEnvelope envelope;
    auto packets = envelope.feed(nullptr, 4);
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "lenprefix feed received null data");
}

BOOST_AUTO_TEST_CASE(FeedInvalidWidthReportsError) {
    EnvelopeConfig config;
    config.length_bytes = 3;
    LenPrefixEnvelope envelope(config);
    const std::vector<std::uint8_t> chunk{0x00, 0x00, 0x00};
    auto packets = envelope.feed(chunk.data(), chunk.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "invalid lenprefix length width");
}

BOOST_AUTO_TEST_CASE(EncodeInvalidWidthReportsError) {
    EnvelopeConfig config;
    config.length_bytes = 3;
    LenPrefixEnvelope envelope(config);

    Packet packet;
    packet.body = bytes("x");
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(), "invalid lenprefix length width");
}

BOOST_AUTO_TEST_CASE(IncludedLengthSmallerThanHeaderFails) {
    EnvelopeConfig config;
    config.length_bytes = 4;
    config.length_includes_header = true;
    LenPrefixEnvelope envelope(config);

    const std::vector<std::uint8_t> frame{0x00, 0x00, 0x00, 0x02};
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "lenprefix frame length overflow");
}

BOOST_AUTO_TEST_CASE(MaxWidthLengthFieldOverflowFails) {
    EnvelopeConfig config;
    config.length_bytes = 8;
    LenPrefixEnvelope envelope(config);

    const auto frame = be_bytes(0xffffffffffffffffULL, 8);
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "lenprefix frame length overflow");
}

BOOST_AUTO_TEST_CASE(FrameTooLargeFails) {
    EnvelopeConfig config;
    config.length_bytes = 4;
    config.max_frame_size = 8;
    LenPrefixEnvelope envelope(config);

    auto frame = concat(be_bytes(10, 4), bytes("0123456789"));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "lenprefix frame too large");
}

BOOST_AUTO_TEST_CASE(HeaderOnlyFrameWaitsForBody) {
    LenPrefixEnvelope envelope;

    const auto header = be_bytes(5, 4);
    BOOST_CHECK(envelope.feed(header.data(), header.size()).empty());
    BOOST_CHECK(envelope.error().empty());

    const auto rest = bytes("12345");
    auto packets = envelope.feed(rest.data(), rest.size());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(),
                                  packets[0].body.end(), rest.begin(),
                                  rest.end());
}

BOOST_AUTO_TEST_CASE(EncodePayloadTooLargeForWidthFails) {
    EnvelopeConfig config;
    config.length_bytes = 1;
    LenPrefixEnvelope envelope(config);

    Packet packet;
    packet.body.assign(300, 0x61);
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(),
                      "lenprefix payload length does not fit header");
}

BOOST_AUTO_TEST_CASE(ResetClearsBufferedDataAndError) {
    EnvelopeConfig config;
    config.length_bytes = 4;
    LenPrefixEnvelope envelope(config);

    const std::vector<std::uint8_t> partial{0x00, 0x00};
    BOOST_CHECK(envelope.feed(partial.data(), partial.size()).empty());
    envelope.reset();
    BOOST_CHECK(envelope.error().empty());

    Packet packet;
    packet.body = bytes("after-reset");
    const auto frame = concat(be_bytes(packet.body.size(), 4), packet.body);
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(),
                                  packets[0].body.end(), packet.body.begin(),
                                  packet.body.end());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// IdLenEnvelope
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovIdLenEnvelope)

BOOST_AUTO_TEST_CASE(DefaultConfigUsesTwoByteFields) {
    IdLenEnvelope envelope;
    BOOST_CHECK(envelope.config().route_source == RouteSource::Header);

    Packet packet;
    packet.route_id = 0x0203;
    packet.body = bytes("d");
    const auto encoded = envelope.encode(packet.ref());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_CHECK_EQUAL(encoded.size(), 5u);
    BOOST_CHECK_EQUAL(encoded[0], 0x02);
    BOOST_CHECK_EQUAL(encoded[1], 0x03);

    auto packets = envelope.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 0x0203u);
}

BOOST_AUTO_TEST_CASE(FeedNullDataReportsError) {
    IdLenEnvelope envelope;
    auto packets = envelope.feed(nullptr, 4);
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "idlen feed received null data");
}

BOOST_AUTO_TEST_CASE(FeedInvalidWidthReportsError) {
    EnvelopeConfig config;
    config.route_id_bytes = 3;
    config.length_bytes = 2;
    IdLenEnvelope envelope(config);
    const std::vector<std::uint8_t> chunk{0x01, 0x02, 0x00, 0x04};
    auto packets = envelope.feed(chunk.data(), chunk.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "invalid idlen integer width");
}

BOOST_AUTO_TEST_CASE(EncodeInvalidWidthReportsError) {
    EnvelopeConfig config;
    config.route_id_bytes = 2;
    config.length_bytes = 3;
    IdLenEnvelope envelope(config);

    Packet packet;
    packet.body = bytes("x");
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(), "invalid idlen integer width");
}

BOOST_AUTO_TEST_CASE(RouteIdOverflowFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 8;
    config.length_bytes = 2;
    IdLenEnvelope envelope(config);

    auto frame = concat(be_bytes(0x100000000ULL, 8), be_bytes(0, 2));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "idlen route id overflow");
}

BOOST_AUTO_TEST_CASE(IncludedLengthSmallerThanHeaderFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 2;
    config.length_bytes = 2;
    config.length_includes_header = true;
    IdLenEnvelope envelope(config);

    auto frame = concat(be_bytes(7, 2), be_bytes(2, 2));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "idlen frame length overflow");
}

BOOST_AUTO_TEST_CASE(MaxWidthLengthFieldOverflowFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 2;
    config.length_bytes = 8;
    IdLenEnvelope envelope(config);

    auto frame = concat(be_bytes(1, 2), be_bytes(0xffffffffffffffffULL, 8));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "idlen frame length overflow");
}

BOOST_AUTO_TEST_CASE(FrameTooLargeFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 2;
    config.length_bytes = 2;
    config.max_frame_size = 4;
    IdLenEnvelope envelope(config);

    auto frame = concat(be_bytes(1, 2), be_bytes(10, 2));
    frame = concat(frame, bytes("0123456789"));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "idlen frame too large");
}

BOOST_AUTO_TEST_CASE(SplitFrameWaitsForRest) {
    EnvelopeConfig config;
    config.endian = Endian::Little;
    config.route_id_bytes = 2;
    config.length_bytes = 2;
    IdLenEnvelope envelope(config);

    const auto frame = idlen_le_frame(0x11, "abcde");
    BOOST_CHECK(envelope.feed(frame.data(), 4).empty());
    BOOST_CHECK(envelope.error().empty());

    auto packets = envelope.feed(frame.data() + 4, frame.size() - 4);
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 0x11u);
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(),
                                  packets[0].body.end(), frame.begin() + 4,
                                  frame.end());
}

BOOST_AUTO_TEST_CASE(EncodeRouteIdTooLargeForWidthFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 2;
    IdLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 0x1ff;
    packet.body = bytes("x");
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(), "idlen route id does not fit header");
}

BOOST_AUTO_TEST_CASE(EncodePayloadTooLargeForWidthFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 1;
    IdLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 1;
    packet.body.assign(300, 0x62);
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(),
                      "idlen payload length does not fit header");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// TypeLenEnvelope
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovTypeLenEnvelope)

BOOST_AUTO_TEST_CASE(DefaultConfigUsesOneByteTypeAndFourByteLength) {
    TypeLenEnvelope envelope;
    BOOST_CHECK(envelope.config().route_source == RouteSource::Header);

    Packet packet;
    packet.route_id = 3;
    packet.body = bytes("zz");
    const auto encoded = envelope.encode(packet.ref());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_CHECK_EQUAL(encoded.size(), 7u);
    BOOST_CHECK_EQUAL(encoded[0], 3u);

    auto packets = envelope.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 3u);
    BOOST_CHECK_EQUAL(packets[0].kind, 3u);
}

BOOST_AUTO_TEST_CASE(FeedNullDataReportsError) {
    TypeLenEnvelope envelope;
    auto packets = envelope.feed(nullptr, 4);
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "typed_len feed received null data");
}

BOOST_AUTO_TEST_CASE(FeedInvalidWidthReportsError) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 5;
    TypeLenEnvelope envelope(config);
    const std::vector<std::uint8_t> chunk{0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    auto packets = envelope.feed(chunk.data(), chunk.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "invalid typed_len integer width");
}

BOOST_AUTO_TEST_CASE(EncodeInvalidWidthReportsError) {
    EnvelopeConfig config;
    config.route_id_bytes = 3;
    config.length_bytes = 4;
    TypeLenEnvelope envelope(config);

    Packet packet;
    packet.body = bytes("x");
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(), "invalid typed_len integer width");
}

BOOST_AUTO_TEST_CASE(TypeIdOverflowFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 8;
    config.length_bytes = 2;
    TypeLenEnvelope envelope(config);

    auto frame = concat(be_bytes(0x100000000ULL, 8), be_bytes(0, 2));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "typed_len type id overflow");
}

BOOST_AUTO_TEST_CASE(IncludedLengthSmallerThanHeaderFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 2;
    config.length_includes_header = true;
    TypeLenEnvelope envelope(config);

    auto frame = concat(be_bytes(1, 1), be_bytes(2, 2));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "typed_len frame length overflow");
}

BOOST_AUTO_TEST_CASE(MaxWidthLengthFieldOverflowFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 8;
    TypeLenEnvelope envelope(config);

    auto frame = concat(be_bytes(1, 1), be_bytes(0xffffffffffffffffULL, 8));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "typed_len frame length overflow");
}

BOOST_AUTO_TEST_CASE(FrameTooLargeFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 2;
    config.max_frame_size = 4;
    TypeLenEnvelope envelope(config);

    auto frame = concat(be_bytes(1, 1), be_bytes(10, 2));
    frame = concat(frame, bytes("0123456789"));
    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "typed_len frame too large");
}

BOOST_AUTO_TEST_CASE(SplitFrameWaitsForRest) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 2;
    TypeLenEnvelope envelope(config);

    auto frame = concat(be_bytes(9, 1), be_bytes(4, 2));
    frame = concat(frame, bytes("body"));
    // Full 3-byte header only: declared body still missing, feed waits.
    BOOST_CHECK(envelope.feed(frame.data(), 3).empty());
    BOOST_CHECK(envelope.error().empty());
    BOOST_CHECK(envelope.feed(frame.data() + 3, 2).empty());
    BOOST_CHECK(envelope.error().empty());

    auto packets = envelope.feed(frame.data() + 5, frame.size() - 5);
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 9u);
}

BOOST_AUTO_TEST_CASE(EncodeFallsBackToPacketKind) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 2;
    TypeLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 0;
    packet.kind = 5;
    packet.body = bytes("k");
    const auto encoded = envelope.encode(packet.ref());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_CHECK_EQUAL(encoded[0], 5u);
}

BOOST_AUTO_TEST_CASE(EncodeTypeIdTooLargeForWidthFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 2;
    TypeLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 0x1ff;
    packet.body = bytes("x");
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(),
                      "typed_len type id does not fit header");
}

BOOST_AUTO_TEST_CASE(EncodePayloadTooLargeForWidthFails) {
    EnvelopeConfig config;
    config.route_id_bytes = 1;
    config.length_bytes = 1;
    TypeLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 1;
    packet.body.assign(300, 0x63);
    BOOST_CHECK(envelope.encode(packet.ref()).empty());
    BOOST_CHECK_EQUAL(envelope.error(),
                      "typed_len payload length does not fit header");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// DelimiterEnvelope
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovDelimiterEnvelope)

BOOST_AUTO_TEST_CASE(CtorStripsIntegerFields) {
    DelimiterEnvelope envelope;
    BOOST_CHECK_EQUAL(envelope.config().length_bytes, 0);
    BOOST_CHECK_EQUAL(envelope.config().route_id_bytes, 0);
    BOOST_CHECK(envelope.config().route_source == RouteSource::Body);
}

BOOST_AUTO_TEST_CASE(FeedNullDataReportsError) {
    DelimiterEnvelope envelope;
    auto packets = envelope.feed(nullptr, 3);
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "delimiter feed received null data");
}

BOOST_AUTO_TEST_CASE(NoDelimiterAndBufferTooLargeFails) {
    EnvelopeConfig config;
    config.delimiter = '\n';
    config.max_frame_size = 4;
    DelimiterEnvelope envelope(config);

    const auto chunk = bytes("abcdef");
    auto packets = envelope.feed(chunk.data(), chunk.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "delimiter frame too large");
}

BOOST_AUTO_TEST_CASE(FrameLongerThanMaxFails) {
    EnvelopeConfig config;
    config.delimiter = '\n';
    config.max_frame_size = 3;
    DelimiterEnvelope envelope(config);

    const auto chunk = bytes("abcd\n");
    auto packets = envelope.feed(chunk.data(), chunk.size());
    BOOST_CHECK(packets.empty());
    BOOST_CHECK_EQUAL(envelope.error(), "delimiter frame too large");
}

BOOST_AUTO_TEST_CASE(EncodeAppendsDelimiter) {
    DelimiterEnvelope envelope;
    Packet packet;
    packet.body = bytes("msg");
    const auto encoded = envelope.encode(packet.ref());
    BOOST_REQUIRE(envelope.error().empty());
    const auto expected = bytes("msg\n");
    BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(),
                                  expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(EmptyFeedKeepsState) {
    DelimiterEnvelope envelope;
    BOOST_CHECK(envelope.feed(nullptr, 0).empty());
    BOOST_CHECK(envelope.error().empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Envelope factories
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovEnvelopeFactory)

BOOST_AUTO_TEST_CASE(StringKindAliasesCreateEachEnvelope) {
    for (const auto* name : {"lenprefix", "len-prefix", "len_prefix", "idlen",
                             "id-len", "id_len", "typed_len", "type_len",
                             "typed-len", "typelen", "delimiter", "line"}) {
        auto envelope = create_envelope(name);
        BOOST_CHECK_MESSAGE(envelope != nullptr, name);
    }
}

BOOST_AUTO_TEST_CASE(UnknownStringKindReturnsNull) {
    BOOST_CHECK(create_envelope("carrier-pigeon") == nullptr);
}

BOOST_AUTO_TEST_CASE(UnknownEnumKindReturnsNull) {
    BOOST_CHECK(create_envelope(static_cast<EnvelopeKind>(99)) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Body codecs
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovBodyCodecs)

BOOST_AUTO_TEST_CASE(RawCodecRouteKeyDefaultsToNull) {
    RawBodyCodec codec;
    Packet packet;
    packet.body = bytes("anything");
    BOOST_CHECK(!codec.route_key(packet.ref()).has_value());
}

BOOST_AUTO_TEST_CASE(JsonCodecRouteKeyReturnsNullOnInvalidJson) {
    JsonBodyCodec codec;
    Packet packet;
    packet.body = bytes("not-json");
    BOOST_CHECK(!codec.route_key(packet.ref()).has_value());

    Packet scalar;
    scalar.body = bytes("42");
    BOOST_CHECK(!codec.route_key(scalar.ref()).has_value());

    Packet array;
    array.body = bytes("[1,2]");
    BOOST_CHECK(!codec.route_key(array.ref()).has_value());
}

BOOST_AUTO_TEST_CASE(RawCodecEncodeAcceptsBytesAndRejectsMessage) {
    RawBodyCodec codec;
    RouteEntry route;
    ProtocolProfile profile;

    DecodedBody raw;
    raw.bytes = bytes("abc");
    const auto out = codec.encode(raw, route, profile);
    BOOST_CHECK_EQUAL_COLLECTIONS(out.begin(), out.end(), raw.bytes.begin(),
                                  raw.bytes.end());

    DecodedBody structured;
    structured.message = nlohmann::json::object();
    BOOST_CHECK_THROW(codec.encode(structured, route, profile),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PassthroughEncodeThrows) {
    auto codec = create_body_codec("protobuf");
    BOOST_REQUIRE(codec != nullptr);
    DecodedBody body;
    body.message = nlohmann::json::object();
    BOOST_CHECK_THROW(codec->encode(body, RouteEntry{}, ProtocolProfile{}),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(FactoryCreatesRawJsonXmldefAndRejectsUnknown) {
    auto raw = create_body_codec("raw");
    BOOST_REQUIRE(raw != nullptr);
    BOOST_CHECK_EQUAL(raw->name(), "raw");

    auto json = create_body_codec("json");
    BOOST_REQUIRE(json != nullptr);
    BOOST_CHECK_EQUAL(json->name(), "json");

    auto xml_def = create_body_codec("xml_def");
    BOOST_REQUIRE(xml_def != nullptr);
    BOOST_CHECK_EQUAL(xml_def->name(), "xmldef");

    BOOST_CHECK(create_body_codec("smoke-signals") == nullptr);
}

BOOST_AUTO_TEST_CASE(JsonEncodePassesThroughRouteHintMessage) {
    JsonBodyCodec codec;
    ProtocolProfile profile;

    RouteEntry route;
    route.route_id = 7;
    route.debug_name = "login";

    DecodedBody body;
    body.message = nlohmann::json{{"route", "custom"}, {"uid", 3}};
    const auto out = codec.encode(body, route, profile);
    const auto parsed = nlohmann::json::parse(out);
    BOOST_CHECK_EQUAL(parsed["route"].get<std::string>(), "custom");
    BOOST_CHECK_EQUAL(parsed["uid"].get<int>(), 3);
    BOOST_CHECK(!parsed.contains("payload"));
}

BOOST_AUTO_TEST_CASE(JsonEncodeWrapsPlainMessageWithRouteMetadata) {
    JsonBodyCodec codec;
    ProtocolProfile profile;

    DecodedBody body;
    body.message = nlohmann::json{{"uid", 1}};

    {
        body.route_name = "named";
        RouteEntry route;
        route.route_id = 7;
        const auto out = codec.encode(body, route, profile);
        const auto parsed = nlohmann::json::parse(out);
        BOOST_CHECK_EQUAL(parsed["route"].get<std::string>(), "named");
        BOOST_CHECK_EQUAL(parsed["route_id"].get<int>(), 7);
        BOOST_CHECK_EQUAL(parsed["payload"]["uid"].get<int>(), 1);
    }

    {
        body.route_name = "";
        RouteEntry route;
        route.route_id = 7;
        route.debug_name = "dbg";
        const auto out = codec.encode(body, route, profile);
        const auto parsed = nlohmann::json::parse(out);
        BOOST_CHECK_EQUAL(parsed["route"].get<std::string>(), "dbg");
    }

    {
        body.route_name = "";
        RouteEntry route;
        const auto out = codec.encode(body, route, profile);
        const auto parsed = nlohmann::json::parse(out);
        BOOST_CHECK(!parsed.contains("route"));
        BOOST_CHECK(!parsed.contains("route_id"));
    }
}

BOOST_AUTO_TEST_CASE(JsonEncodeEmptyBodyBuildsEnvelopeFromRoute) {
    JsonBodyCodec codec;
    ProtocolProfile profile;

    DecodedBody body;

    {
        body.route_name = "named";
        RouteEntry route;
        route.route_id = 9;
        const auto out = codec.encode(body, route, profile);
        const auto parsed = nlohmann::json::parse(out);
        BOOST_CHECK_EQUAL(parsed["route"].get<std::string>(), "named");
        BOOST_CHECK_EQUAL(parsed["route_id"].get<int>(), 9);
        BOOST_CHECK(parsed["payload"].is_object());
    }

    {
        body.route_name = "";
        RouteEntry route;
        route.route_id = 4;
        route.debug_name = "fallback";
        const auto out = codec.encode(body, route, profile);
        const auto parsed = nlohmann::json::parse(out);
        BOOST_CHECK_EQUAL(parsed["route"].get<std::string>(), "fallback");
        BOOST_CHECK_EQUAL(parsed["route_id"].get<int>(), 4);
    }
}

BOOST_AUTO_TEST_CASE(JsonDecodeReadsMsgIdAndMethodKeys) {
    JsonBodyCodec codec;

    RouteEntry by_id;
    by_id.route_id = 1001;
    by_id.codec_id = 2;
    by_id.schema_id = 3;

    Packet id_packet;
    id_packet.body = bytes(R"({"msg_id":1001,"payload":{"ok":true}})");
    auto decoded = codec.decode(id_packet.ref(), by_id);
    BOOST_CHECK_EQUAL(decoded.route_id, 1001u);
    BOOST_CHECK_EQUAL(decoded.codec_id, 2u);
    BOOST_CHECK_EQUAL(decoded.schema_id, 3u);
    BOOST_REQUIRE(decoded.has_message());
    BOOST_CHECK((*decoded.message)["ok"].get<bool>());

    RouteEntry by_name;
    by_name.route_id = 1002;
    Packet name_packet;
    name_packet.body = bytes(R"({"method":"ping"})");
    decoded = codec.decode(name_packet.ref(), by_name);
    BOOST_CHECK_EQUAL(decoded.route_name, "ping");
    BOOST_REQUIRE(decoded.has_message());
    BOOST_CHECK(!decoded.message->contains("payload"));
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// ExternalBodyCodec
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovExternalBodyCodec)

RouteEntry ext_route() {
    RouteEntry route;
    route.route_id = 5;
    route.codec_id = 2;
    route.schema_id = 3;
    route.debug_name = "fight";
    return route;
}

BOOST_AUTO_TEST_CASE(NullCodecPointerIsRejected) {
    ExternalBodyCodec codec("prov", "protobuf", nullptr);
    Packet packet;
    packet.body = bytes("x");
    BOOST_CHECK_THROW(codec.decode(packet.ref(), ext_route()),
                      std::runtime_error);
    DecodedBody body;
    body.message = nlohmann::json::object();
    BOOST_CHECK_THROW(codec.encode(body, ext_route(), ProtocolProfile{}),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(IncompleteVtableIsRejected) {
    FakeExtState state;
    auto no_decode = make_ext_codec(state, false, true);
    ExternalBodyCodec decode_missing("prov", "protobuf", &no_decode);
    Packet packet;
    BOOST_CHECK_THROW(decode_missing.decode(packet.ref(), ext_route()),
                      std::runtime_error);

    auto no_encode = make_ext_codec(state, true, false);
    ExternalBodyCodec encode_missing("prov", "protobuf", &no_encode);
    DecodedBody body;
    body.message = nlohmann::json::object();
    BOOST_CHECK_THROW(
        encode_missing.encode(body, ext_route(), ProtocolProfile{}),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DecodeFailurePrefersPluginErrorMessage) {
    FakeExtState state;
    state.decode_rc = -1;
    state.err_message = "boom detail";
    state.err_code = "E_BOOM";
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    Packet packet;
    BOOST_CHECK_THROW(codec.decode(packet.ref(), ext_route()),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DecodeFailureFallsBackToCodeThenDefault) {
    FakeExtState state;
    state.decode_rc = -1;
    state.err_message = "";
    state.err_code = "E_CODE";
    auto with_code = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &with_code);
    Packet packet;
    BOOST_CHECK_THROW(codec.decode(packet.ref(), ext_route()),
                      std::runtime_error);

    state.err_code = "";
    auto bare = make_ext_codec(state);
    ExternalBodyCodec codec2("prov", "protobuf", &bare);
    BOOST_CHECK_THROW(codec2.decode(packet.ref(), ext_route()),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DecodeCopiesRouteMetadataAndBytes) {
    FakeExtState state;
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    Packet packet;
    packet.route_id = 5;
    packet.body = bytes("wire");
    const auto decoded = codec.decode(packet.ref(), ext_route());
    BOOST_CHECK_EQUAL(decoded.route_id, 5u);
    BOOST_CHECK_EQUAL(decoded.codec_id, 2u);
    BOOST_CHECK_EQUAL(decoded.schema_id, 3u);
    BOOST_CHECK_EQUAL(decoded.route_name, "fight");
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.bytes.begin(), decoded.bytes.end(),
                                  packet.body.begin(), packet.body.end());
    BOOST_REQUIRE(decoded.has_message());
    BOOST_CHECK_EQUAL((*decoded.message)["uid"].get<int>(), 7);
    BOOST_CHECK_EQUAL(state.last_route_id, 5u);
    BOOST_CHECK_EQUAL(state.last_decode_route_name, "fight");
}

BOOST_AUTO_TEST_CASE(DecodeNullMessageYieldsEmptyObject) {
    FakeExtState state;
    state.null_message = true;
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    Packet packet;
    packet.body = bytes("wire");
    const auto decoded = codec.decode(packet.ref(), ext_route());
    BOOST_REQUIRE(decoded.has_message());
    BOOST_CHECK(decoded.message->is_object());
    BOOST_CHECK(decoded.message->empty());
}

BOOST_AUTO_TEST_CASE(DecodeInvalidJsonRethrowsAfterFree) {
    FakeExtState state;
    state.decoded_json = "{not-valid-json";
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    Packet packet;
    packet.body = bytes("wire");
    BOOST_CHECK_THROW(codec.decode(packet.ref(), ext_route()),
                      nlohmann::json::exception);
}

BOOST_AUTO_TEST_CASE(EncodeFailureSurfacesPluginError) {
    FakeExtState state;
    state.encode_rc = -2;
    state.err_message = "encode blew up";
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    DecodedBody body;
    body.message = nlohmann::json::object();
    BOOST_CHECK_THROW(codec.encode(body, ext_route(), ProtocolProfile{}),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(EncodeUnwrapsPayloadAndHandlesEmptyBody) {
    FakeExtState state;
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    DecodedBody wrapped;
    wrapped.message = nlohmann::json{{"payload", nlohmann::json{{"uid", 2}}},
                                     {"route_id", 5}};
    auto out = codec.encode(wrapped, ext_route(), ProtocolProfile{});
    const auto expected = bytes("ENC");
    BOOST_CHECK_EQUAL_COLLECTIONS(out.begin(), out.end(), expected.begin(),
                                  expected.end());
    const auto input = nlohmann::json::parse(state.last_encode_input);
    BOOST_CHECK_EQUAL(input["uid"].get<int>(), 2);
    BOOST_CHECK(!input.contains("payload"));

    DecodedBody empty;
    BOOST_CHECK_NO_THROW(codec.encode(empty, ext_route(), ProtocolProfile{}));
    BOOST_CHECK_EQUAL(state.last_encode_input, "{}");

    DecodedBody raw_bytes;
    raw_bytes.bytes = bytes("raw");
    BOOST_CHECK_THROW(codec.encode(raw_bytes, ext_route(), ProtocolProfile{}),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(EncodeFreeThrowingIsPropagated) {
    FakeExtState state;
    state.free_encode_throws = true;
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    DecodedBody body;
    body.message = nlohmann::json::object();
    BOOST_CHECK_THROW(codec.encode(body, ext_route(), ProtocolProfile{}),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(EncodeFreeThrowingOnceIsRethrown) {
    FakeExtState state;
    state.free_encode_throw_once = true;
    auto fake = make_ext_codec(state);
    ExternalBodyCodec codec("prov", "protobuf", &fake);

    DecodedBody body;
    body.message = nlohmann::json::object();
    bool threw = false;
    try {
        (void)codec.encode(body, ext_route(), ProtocolProfile{});
    } catch (const std::runtime_error& ex) {
        threw = true;
        BOOST_CHECK_NE(std::string(ex.what()).find("free_encode_result"),
                       std::string::npos);
    }
    BOOST_CHECK(threw);
    BOOST_CHECK(!state.free_encode_throw_once);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// xmldef route catalog
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovXmldefLoader)

bool load_ok(std::string_view xml, RouteTable& routes, std::string* error) {
    return load_xmldef_routes_from_string(xml, routes, {}, error);
}

BOOST_AUTO_TEST_CASE(SkipsNonMessageTagsAndSpecialPrefixes) {
    RouteTable routes;
    std::string error;
    const auto xml =
        "<protocol name='arena'><?xml v?><!-- note --><entity id='9'/>"
        "<message id=\" 7 \" name=\"gap\"/>"
        "</protocol>";
    BOOST_REQUIRE(load_ok(xml, routes, &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(routes.size(), 1u);
    BOOST_REQUIRE(routes.find(7) != nullptr);
    BOOST_CHECK_EQUAL(routes.find(7)->debug_name, "gap");
}

BOOST_AUTO_TEST_CASE(UnterminatedTagFails) {
    RouteTable routes;
    std::string error;
    BOOST_CHECK(!load_ok("<message id=\"1\"", routes, &error));
    BOOST_CHECK_NE(error.find("unterminated tag"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(MissingIdFails) {
    RouteTable routes;
    std::string error;
    BOOST_CHECK(!load_ok("<message name=\"x\"/>", routes, &error));
    BOOST_CHECK_NE(error.find("missing id"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(UnquotedIdAttributeIsDropped) {
    RouteTable routes;
    std::string error;
    BOOST_CHECK(!load_ok("<message id=1/>", routes, &error));
    BOOST_CHECK_NE(error.find("missing id"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(UnterminatedQuoteDropsAttribute) {
    RouteTable routes;
    std::string error;
    BOOST_CHECK(
        load_ok("<message id=\"1\" name=\"dangling />", routes, &error));
    BOOST_REQUIRE(routes.find(1) != nullptr);
    BOOST_CHECK(routes.find(1)->debug_name.empty());
}

BOOST_AUTO_TEST_CASE(BareAndSpacedAttributesParse) {
    RouteTable routes;
    std::string error;
    BOOST_CHECK(
        load_ok("<message id=\"2\" bare sealed = \"true\" />", routes, &error));
    BOOST_REQUIRE(routes.find(2) != nullptr);
}

BOOST_AUTO_TEST_CASE(InvalidIdValuesFail) {
    const std::string_view cases[] = {
        "zero", "0", "4294967296", "99999999999999999999999", "12ab", "   "};
    for (const auto& id : cases) {
        RouteTable routes;
        std::string error;
        const auto xml = "<message id=\"" + std::string(id) + "\"/>";
        BOOST_CHECK_MESSAGE(!load_ok(xml, routes, &error), id);
        BOOST_CHECK_NE(error.find("uint32"), std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(DirectionAliasesAndInvalidValue) {
    RouteTable routes;
    std::string error;
    BOOST_REQUIRE(
        load_ok("<message id=\"1\" direction=\"server_to_client\"/>"
                "<message id=\"2\" direction=\"bidi\"/>",
                routes, &error));
    BOOST_CHECK(routes.find(1)->direction == RouteDirection::ServerToClient);
    BOOST_CHECK(routes.find(2)->direction == RouteDirection::Bidirectional);

    RouteTable bad;
    BOOST_CHECK(
        !load_ok("<message id=\"3\" direction=\"north\"/>", bad, &error));
    BOOST_CHECK_NE(error.find("direction"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RequiresAuthVariants) {
    RouteTable routes;
    std::string error;
    BOOST_REQUIRE(
        load_ok("<message id=\"1\" requires_auth=\"0\"/>"
                "<message id=\"2\" requires_auth=\"true\"/>",
                routes, &error));
    BOOST_CHECK(!routes.find(1)->requires_auth);
    BOOST_CHECK(routes.find(2)->requires_auth);
}

BOOST_AUTO_TEST_CASE(CodecAndSchemaAttrErrors) {
    std::string error;
    {
        RouteTable routes;
        BOOST_CHECK(
            !load_ok("<message id=\"1\" codec_id=\"65536\"/>", routes, &error));
        BOOST_CHECK_NE(error.find("codec_id"), std::string::npos);
    }
    {
        RouteTable routes;
        BOOST_CHECK(
            !load_ok("<message id=\"1\" schema_id=\"xx\"/>", routes, &error));
        BOOST_CHECK_NE(error.find("schema_id"), std::string::npos);
    }
    {
        RouteTable routes;
        BOOST_CHECK(
            !load_ok("<message id=\"1\" schema=\"bad\"/>", routes, &error));
        BOOST_CHECK_NE(error.find("schema"), std::string::npos);
    }
    {
        RouteTable routes;
        BOOST_CHECK(load_ok("<message id=\"1\" codec_id=\"9\" schema=\"8\"/>",
                            routes, &error));
        BOOST_CHECK_EQUAL(routes.find(1)->codec_id, 9u);
        BOOST_CHECK_EQUAL(routes.find(1)->schema_id, 8u);
    }
}

BOOST_AUTO_TEST_CASE(ActionDropAndInvalidAction) {
    std::string error;
    {
        RouteTable routes;
        BOOST_REQUIRE(
            load_ok("<message id=\"1\" action=\"drop\"/>", routes, &error));
        BOOST_CHECK(routes.find(1)->policy.action == RouteAction::Drop);
    }
    {
        RouteTable routes;
        BOOST_CHECK(
            !load_ok("<message id=\"1\" action=\"explode\"/>", routes, &error));
        BOOST_CHECK_NE(error.find("action"), std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(LazyDecodeBoolValues) {
    std::string error;
    {
        RouteTable routes;
        BOOST_REQUIRE(
            load_ok("<message id=\"1\" lazy_decode=\"yes\"/>", routes, &error));
        BOOST_CHECK(routes.find(1)->policy.lazy_decode);
    }
    {
        RouteTable routes;
        BOOST_REQUIRE(
            load_ok("<message id=\"1\" lazy_decode=\"no\"/>", routes, &error));
        BOOST_CHECK(!routes.find(1)->policy.lazy_decode);
    }
    {
        RouteTable routes;
        BOOST_CHECK(!load_ok("<message id=\"1\" lazy_decode=\"maybe\"/>",
                             routes, &error));
        BOOST_CHECK_NE(error.find("lazy_decode"), std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(MethodAndLogicalServiceAttributes) {
    RouteTable routes;
    std::string error;
    BOOST_REQUIRE(load_ok(
        "<message id=\"1\" method=\"do_login\" logical_service=\"auth\"/>",
        routes, &error));
    const auto* route = routes.find(1);
    BOOST_REQUIRE(route != nullptr);
    BOOST_REQUIRE(route->method_name.has_value());
    BOOST_CHECK_EQUAL(*route->method_name, "do_login");
    BOOST_REQUIRE(route->logical_service_name.has_value());
    BOOST_CHECK_EQUAL(*route->logical_service_name, "auth");
}

BOOST_AUTO_TEST_CASE(DefaultOptionsApplyToEntries) {
    RouteTable routes;
    std::string error;
    XmldefCatalogOptions options;
    options.default_codec_id = 4;
    options.default_action = RouteAction::ForwardRaw;
    options.default_lazy_decode = false;
    BOOST_REQUIRE(load_xmldef_routes_from_string("<message id=\"1\"/>", routes,
                                                 options, &error));
    const auto* route = routes.find(1);
    BOOST_REQUIRE(route != nullptr);
    BOOST_CHECK_EQUAL(route->codec_id, 4u);
    BOOST_CHECK(route->policy.action == RouteAction::ForwardRaw);
    BOOST_CHECK(!route->policy.lazy_decode);
}

BOOST_AUTO_TEST_CASE(DuplicateRouteIdFails) {
    RouteTable routes;
    std::string error;
    BOOST_CHECK(
        !load_ok("<message id=\"7\"/><route id=\"7\"/>", routes, &error));
    BOOST_CHECK_NE(error.find("duplicate"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(FileLoadHandlesMissingAndExistingFiles) {
    RouteTable routes;
    std::string error;
    BOOST_CHECK(!load_xmldef_routes_from_file(
        "/nonexistent/shield/cov/missing.xml", routes, {}, &error));
    BOOST_CHECK_NE(error.find("failed to open"), std::string::npos);

    const auto dir = cov_temp_dir();
    const auto catalog = dir / "cov_messages.xml";
    {
        std::ofstream out(catalog);
        out << "<protocol><message id=\"0x20\" "
               "name=\"file.loaded\"/></protocol>";
    }
    RouteTable loaded;
    std::string load_error;
    BOOST_REQUIRE(load_xmldef_routes_from_file(catalog.string(), loaded, {},
                                               &load_error));
    BOOST_REQUIRE(loaded.find(0x20) != nullptr);
    BOOST_CHECK_EQUAL(loaded.find(0x20)->debug_name, "file.loaded");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// RouteTable / BodyCodecRegistry
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovRouteAndRegistry)

BOOST_AUTO_TEST_CASE(RouteTableUpsertReplacesEntryAndName) {
    RouteTable routes;
    RouteEntry first;
    first.route_id = 1;
    first.debug_name = "alpha";
    BOOST_REQUIRE(routes.add(first));

    RouteEntry second;
    second.route_id = 1;
    second.debug_name = "beta";
    routes.upsert(second);

    BOOST_CHECK_EQUAL(routes.size(), 1u);
    BOOST_REQUIRE(routes.find(1) != nullptr);
    BOOST_CHECK_EQUAL(routes.find(1)->debug_name, "beta");
    BOOST_CHECK(routes.find_by_name("alpha") == nullptr);
    BOOST_REQUIRE(routes.find_by_name("beta") != nullptr);
    BOOST_CHECK_EQUAL(routes.find_by_name("beta")->route_id, 1u);

    RouteEntry unnamed;
    unnamed.route_id = 2;
    routes.upsert(unnamed);
    BOOST_CHECK_EQUAL(routes.size(), 2u);

    RouteEntry named3;
    named3.route_id = 3;
    named3.debug_name = "gamma";
    routes.upsert(named3);
    BOOST_CHECK_EQUAL(routes.size(), 3u);
}

BOOST_AUTO_TEST_CASE(RouteTableOnlyRequiresExactlyOneRoute) {
    RouteTable empty;
    BOOST_CHECK(empty.only() == nullptr);

    RouteTable several;
    RouteEntry a;
    a.route_id = 1;
    RouteEntry b;
    b.route_id = 2;
    BOOST_REQUIRE(several.add(a));
    BOOST_REQUIRE(several.add(b));
    BOOST_CHECK(several.only() == nullptr);

    RouteTable single;
    RouteEntry only_route;
    only_route.route_id = 42;
    only_route.debug_name = "solo";
    BOOST_REQUIRE(single.add(only_route));
    BOOST_REQUIRE(single.only() != nullptr);
    BOOST_CHECK_EQUAL(single.only()->route_id, 42u);
}

BOOST_AUTO_TEST_CASE(RouteTableClearResetsState) {
    RouteTable routes;
    RouteEntry entry;
    entry.route_id = 5;
    entry.debug_name = "five";
    BOOST_REQUIRE(routes.add(entry));
    BOOST_CHECK_EQUAL(routes.size(), 1u);

    routes.clear();
    BOOST_CHECK_EQUAL(routes.size(), 0u);
    BOOST_CHECK(routes.find(5) == nullptr);
    BOOST_CHECK(routes.find_by_name("five") == nullptr);
    BOOST_CHECK(!routes.contains(5));
}

BOOST_AUTO_TEST_CASE(RegistryAddRejectsNullDuplicateIdAndDuplicateName) {
    BodyCodecRegistry registry;
    BOOST_CHECK(!registry.add(1, nullptr));

    BOOST_REQUIRE(registry.add(1, std::make_unique<RawBodyCodec>()));
    BOOST_CHECK(!registry.add(1, std::make_unique<JsonBodyCodec>()));  // dup id
    BOOST_CHECK_EQUAL(registry.size(), 1u);

    BOOST_REQUIRE(registry.add(2, std::make_unique<JsonBodyCodec>()));
    // duplicate name "raw" under a fresh id must fail
    BOOST_CHECK(!registry.add(3, std::make_unique<RawBodyCodec>()));
    BOOST_CHECK_EQUAL(registry.size(), 2u);
}

BOOST_AUTO_TEST_CASE(RegistryUpsertReplacesAndCleansNames) {
    BodyCodecRegistry registry;
    BOOST_REQUIRE(registry.add(1, std::make_unique<RawBodyCodec>()));

    registry.upsert(1, nullptr);  // no-op
    BOOST_REQUIRE(registry.find(1) != nullptr);
    BOOST_CHECK_EQUAL(registry.find(1)->name(), "raw");

    registry.upsert(1, std::make_unique<JsonBodyCodec>());
    BOOST_REQUIRE(registry.find(1) != nullptr);
    BOOST_CHECK_EQUAL(registry.find(1)->name(), "json");
    BOOST_CHECK(registry.find_by_name("raw") == nullptr);
    BOOST_REQUIRE(registry.find_by_name("json") != nullptr);

    // upsert over an unnamed codec leaves the name map alone
    BOOST_REQUIRE(registry.add(2, std::make_unique<PassthroughBodyCodec>("")));
    registry.upsert(2, std::make_unique<RawBodyCodec>());
    BOOST_REQUIRE(registry.find(2) != nullptr);
    BOOST_CHECK_EQUAL(registry.find(2)->name(), "raw");
}

BOOST_AUTO_TEST_CASE(RegistryFindAndNameLookupAllOverloads) {
    BodyCodecRegistry registry;
    BOOST_REQUIRE(registry.add(1, std::make_unique<RawBodyCodec>()));
    BOOST_REQUIRE(registry.add(7, std::make_unique<JsonBodyCodec>()));

    BOOST_CHECK(registry.find(9) == nullptr);
    BOOST_CHECK(registry.find_by_name("xmldef") == nullptr);

    const auto& view = registry;
    BOOST_CHECK(view.find(9) == nullptr);
    BOOST_CHECK(view.find_by_name("xmldef") == nullptr);
    BOOST_REQUIRE(view.find(1) != nullptr);
    BOOST_CHECK_EQUAL(view.find(1)->name(), "raw");
    BOOST_REQUIRE(view.find_by_name("json") != nullptr);
    BOOST_CHECK_EQUAL(view.find_by_name("json")->name(), "json");

    BOOST_CHECK_EQUAL(registry.size(), 2u);
    registry.clear();
    BOOST_CHECK_EQUAL(registry.size(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// ProtocolPipeline dispatch
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovPipelineDispatch)

ProtocolPipeline make_idlen_pipeline(RouteTable routes,
                                     BodyCodecRegistry codecs,
                                     ProtocolProfile& profile) {
    profile.envelope_kind = EnvelopeKind::IdLen;
    profile.envelope.endian = Endian::Little;
    profile.envelope.route_id_bytes = 2;
    profile.envelope.length_bytes = 2;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Header;
    return ProtocolPipeline(profile, std::move(routes), std::move(codecs));
}

BOOST_AUTO_TEST_CASE(PipelineWithoutEnvelopeSurfacesErrorsEverywhere) {
    ProtocolProfile bad;
    bad.envelope_kind = static_cast<EnvelopeKind>(99);
    ProtocolPipeline pipeline(bad, RouteTable{}, BodyCodecRegistry{});
    BOOST_CHECK_NE(pipeline.error().find("failed to create protocol envelope"),
                   std::string::npos);

    auto results = pipeline.feed(nullptr, 0);
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK_EQUAL(results[0].error, "protocol envelope is not configured");

    BOOST_CHECK(pipeline.encode(PacketRef{}).empty());
    BOOST_CHECK_NE(pipeline.error().find("not configured"), std::string::npos);

    BOOST_CHECK(pipeline.encode_message(DecodedBody{}).empty());
    BOOST_CHECK_NE(pipeline.error().find("not configured"), std::string::npos);

    BOOST_CHECK_NO_THROW(pipeline.reset());
    BOOST_CHECK(pipeline.error().empty());
}

BOOST_AUTO_TEST_CASE(DefaultCodecNameEmptyWhenCodecMissing) {
    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    ProtocolPipeline pipeline(profile, RouteTable{}, BodyCodecRegistry{});
    BOOST_CHECK(pipeline.default_codec_name().empty());
}

BOOST_AUTO_TEST_CASE(EnvelopeFeedErrorBecomesDispatchError) {
    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.envelope.length_bytes = 4;
    profile.envelope.max_frame_size = 8;
    ProtocolPipeline pipeline(profile, RouteTable{}, BodyCodecRegistry{});

    auto frame = concat(be_bytes(20, 4), bytes("0123456789ABCDEFGHIJ"));
    auto results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(!results[0].ok());
    BOOST_CHECK_NE(results[0].error.find("too large"), std::string::npos);
    BOOST_CHECK(!pipeline.materialize_decode(results[0]));
}

BOOST_AUTO_TEST_CASE(BodyRouteWithoutDefaultCodecReportsError) {
    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    ProtocolPipeline pipeline(profile, RouteTable{}, BodyCodecRegistry{});

    const auto body = bytes(R"({"route":"login"})");
    auto frame = concat(be_bytes(body.size(), 4), body);
    auto results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(!results[0].ok());
    BOOST_CHECK_NE(results[0].error.find("default body codec"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(BodyRouteKeyWithoutHintYieldsUnknownRoute) {
    RouteTable routes;
    RouteEntry entry;
    entry.route_id = 1001;
    entry.debug_name = "login";
    entry.policy = RoutePolicy{RouteAction::DecodeLocal, false};
    BOOST_REQUIRE(routes.add(entry));

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    const auto body = bytes(R"({"uid":1})");
    auto frame = concat(be_bytes(body.size(), 4), body);
    auto results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_CHECK(results[0].route == nullptr);
    BOOST_CHECK(results[0].should_drop());

    const auto unknown_name = bytes(R"({"route":"missing"})");
    frame = concat(be_bytes(unknown_name.size(), 4), unknown_name);
    results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_CHECK(results[0].route == nullptr);
}

BOOST_AUTO_TEST_CASE(DropRouteActionSkipsDecode) {
    RouteTable routes;
    RouteEntry entry;
    entry.route_id = 0x50;
    entry.policy.action = RouteAction::Drop;
    BOOST_REQUIRE(routes.add(entry));

    ProtocolProfile profile;
    auto pipeline =
        make_idlen_pipeline(std::move(routes), BodyCodecRegistry{}, profile);

    const auto frame = idlen_le_frame(0x50, "whatever");
    auto results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_CHECK(results[0].should_drop());
    BOOST_CHECK(!results[0].decoded());
    BOOST_REQUIRE(results[0].route != nullptr);

    // materialize on a Drop / non-decode_local result is a no-op
    BOOST_CHECK(pipeline.materialize_decode(results[0]));
    BOOST_CHECK(!results[0].decoded());
}

BOOST_AUTO_TEST_CASE(DecodeLocalWithoutRegisteredCodecFails) {
    RouteTable routes;
    RouteEntry entry;
    entry.route_id = 0x51;
    entry.policy = RoutePolicy{RouteAction::DecodeLocal, false};
    BOOST_REQUIRE(routes.add(entry));

    ProtocolProfile profile;
    auto pipeline =
        make_idlen_pipeline(std::move(routes), BodyCodecRegistry{}, profile);

    const auto frame = idlen_le_frame(0x51, "data");
    auto results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(!results[0].ok());
    BOOST_CHECK_NE(results[0].error.find("body codec is not registered"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(DecodeFailureIsCapturedInResult) {
    RouteTable routes;
    RouteEntry entry;
    entry.route_id = 0x52;
    entry.policy = RoutePolicy{RouteAction::DecodeLocal, false};
    BOOST_REQUIRE(routes.add(entry));

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    auto pipeline =
        make_idlen_pipeline(std::move(routes), std::move(codecs), profile);

    const auto frame = idlen_le_frame(0x52, "not-json");
    auto results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(!results[0].ok());
    BOOST_CHECK_NE(results[0].error.find("body decode failed"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(MaterializeDecodeBranches) {
    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    // 1. missing route metadata for lazy decode_local dispatch
    {
        ProtocolProfile profile;
        profile.envelope_kind = EnvelopeKind::IdLen;
        profile.envelope.endian = Endian::Little;
        profile.envelope.route_id_bytes = 2;
        profile.envelope.length_bytes = 2;
        profile.default_codec_id = 1;
        profile.route_source = RouteSource::Header;
        profile.unknown_route_action = RouteAction::DecodeLocal;
        ProtocolPipeline pipeline(profile, RouteTable{}, BodyCodecRegistry{});

        const auto frame = idlen_le_frame(0x99, "x");
        auto results = pipeline.feed(frame.data(), frame.size());
        BOOST_REQUIRE_EQUAL(results.size(), 1u);
        BOOST_CHECK(results[0].ok());
        BOOST_CHECK(!pipeline.materialize_decode(results[0]));
        BOOST_CHECK_NE(results[0].error.find("missing route metadata"),
                       std::string::npos);
    }

    // 2. codec missing at materialize time
    {
        RouteTable routes;
        RouteEntry entry;
        entry.route_id = 0x61;
        entry.policy = RoutePolicy{RouteAction::DecodeLocal, true};
        BOOST_REQUIRE(routes.add(entry));

        ProtocolProfile profile;
        auto pipeline = make_idlen_pipeline(std::move(routes),
                                            BodyCodecRegistry{}, profile);

        const auto frame = idlen_le_frame(0x61, "{}");
        auto results = pipeline.feed(frame.data(), frame.size());
        BOOST_REQUIRE_EQUAL(results.size(), 1u);
        BOOST_CHECK(results[0].ok());
        BOOST_CHECK(!results[0].decoded());
        BOOST_CHECK(!pipeline.materialize_decode(results[0]));
        BOOST_CHECK_NE(results[0].error.find("body codec is not registered"),
                       std::string::npos);
    }

    // 3. decode throws at materialize time
    {
        RouteTable routes;
        RouteEntry entry;
        entry.route_id = 0x62;
        entry.policy = RoutePolicy{RouteAction::DecodeLocal, true};
        BOOST_REQUIRE(routes.add(entry));

        ProtocolProfile profile;
        auto pipeline =
            make_idlen_pipeline(std::move(routes), std::move(codecs), profile);

        const auto frame = idlen_le_frame(0x62, "garbage");
        auto results = pipeline.feed(frame.data(), frame.size());
        BOOST_REQUIRE_EQUAL(results.size(), 1u);
        BOOST_CHECK(results[0].ok());
        BOOST_CHECK(!pipeline.materialize_decode(results[0]));
        BOOST_CHECK_NE(results[0].error.find("body decode failed"),
                       std::string::npos);
    }

    // 4. already decoded result short-circuits
    {
        RouteTable routes;
        RouteEntry entry;
        entry.route_id = 0x63;
        entry.policy = RoutePolicy{RouteAction::DecodeLocal, false};
        BOOST_REQUIRE(routes.add(entry));

        ProtocolProfile profile;
        BodyCodecRegistry fresh_codecs;
        fresh_codecs.add(1, std::make_unique<JsonBodyCodec>());
        auto pipeline = make_idlen_pipeline(std::move(routes),
                                            std::move(fresh_codecs), profile);

        const auto frame = idlen_le_frame(0x63, R"({"uid":1})");
        auto results = pipeline.feed(frame.data(), frame.size());
        BOOST_REQUIRE_EQUAL(results.size(), 1u);
        BOOST_CHECK(results[0].decoded());
        BOOST_CHECK(pipeline.materialize_decode(results[0]));
    }
}

BOOST_AUTO_TEST_CASE(EncodeSurfacesEnvelopeErrors) {
    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.envelope.length_bytes = 1;
    ProtocolPipeline pipeline(profile, RouteTable{}, BodyCodecRegistry{});

    Packet packet;
    packet.body.assign(300, 0x61);
    BOOST_CHECK(pipeline.encode(packet.ref()).empty());
    BOOST_CHECK_NE(pipeline.error().find("does not fit header"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(ResetClearsEnvelopeBuffer) {
    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.envelope.length_bytes = 4;
    ProtocolPipeline pipeline(profile, RouteTable{}, BodyCodecRegistry{});

    const auto partial = bytes("\x00\x00");
    BOOST_CHECK(pipeline.feed(partial.data(), partial.size()).empty());
    pipeline.reset();
    BOOST_CHECK(pipeline.error().empty());

    const auto body = bytes("ok");
    auto frame = concat(be_bytes(body.size(), 4), body);
    auto results = pipeline.feed(frame.data(), frame.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// ProtocolPipeline outbound (encode_message) routing
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovPipelineOutbound)

std::unique_ptr<ProtocolPipeline> make_outbound_pipeline(RouteTable routes) {
    BodyCodecRegistry codecs;
    codecs.add(1, std::make_unique<JsonBodyCodec>());

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.envelope.length_bytes = 4;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    return std::make_unique<ProtocolPipeline>(profile, std::move(routes),
                                              std::move(codecs));
}

RouteTable two_routes() {
    RouteTable routes;
    RouteEntry a;
    a.route_id = 1001;
    a.debug_name = "login";
    routes.upsert(a);
    RouteEntry b;
    b.route_id = 1002;
    b.debug_name = "ping";
    routes.upsert(b);
    return routes;
}

BOOST_AUTO_TEST_CASE(ResolveByNameSetsRouteId) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.route_name = "login";
    const auto frame = pipeline->encode_message(body);
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    BOOST_CHECK(!frame.empty());
    BOOST_REQUIRE(pipeline->routes().find(1001) != nullptr);
}

BOOST_AUTO_TEST_CASE(ResolveByMessageRouteId) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.message = nlohmann::json{{"route_id", 1001u}};
    const auto frame = pipeline->encode_message(body);
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    BOOST_CHECK(!frame.empty());
}

BOOST_AUTO_TEST_CASE(ResolveByMessageMsgId) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.message = nlohmann::json{{"msg_id", 1002u}};
    const auto frame = pipeline->encode_message(body);
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    BOOST_CHECK(!frame.empty());
}

BOOST_AUTO_TEST_CASE(ResolveByMessageRouteString) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.message = nlohmann::json{{"route", "login"}};
    const auto frame = pipeline->encode_message(body);
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    BOOST_CHECK(!frame.empty());
}

BOOST_AUTO_TEST_CASE(ResolveByMessageMethodString) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.message = nlohmann::json{{"method", "ping"}};
    const auto frame = pipeline->encode_message(body);
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    BOOST_CHECK(!frame.empty());
}

BOOST_AUTO_TEST_CASE(ResolveByMessageRouteIdMissThenMsgIdMissFails) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.message = nlohmann::json{{"route_id", 9999u}, {"msg_id", 8888u}};
    BOOST_CHECK(pipeline->encode_message(body).empty());
    BOOST_CHECK_NE(pipeline->error().find("failed to resolve outbound route"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(ResolveThroughSingleRouteWhenNoHints) {
    RouteTable routes;
    RouteEntry only_route;
    only_route.route_id = 77;
    only_route.debug_name = "solo";
    routes.upsert(only_route);

    auto pipeline = make_outbound_pipeline(std::move(routes));
    DecodedBody body;
    body.message = nlohmann::json{{"uid", 5}};
    const auto frame = pipeline->encode_message(body);
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    BOOST_CHECK(!frame.empty());
}

BOOST_AUTO_TEST_CASE(ResolveFailsWithHintAndNoMatch) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.route_id = 9999;
    BOOST_CHECK(pipeline->encode_message(body).empty());
    BOOST_CHECK_NE(pipeline->error().find("failed to resolve outbound route"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(EncodeMessageFailsWhenCodecMissing) {
    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.envelope.length_bytes = 4;
    profile.default_codec_id = 1;

    RouteTable routes;
    RouteEntry entry;
    entry.route_id = 1001;
    routes.upsert(entry);

    ProtocolPipeline pipeline(profile, std::move(routes), BodyCodecRegistry{});
    DecodedBody body;
    body.route_id = 1001;
    BOOST_CHECK(pipeline.encode_message(body).empty());
    BOOST_CHECK_NE(pipeline.error().find("body codec is not registered"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(EncodeMessageCapturesCodecThrow) {
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    body.route_id = 1001;
    body.bytes = bytes("raw-bytes");
    BOOST_CHECK(pipeline->encode_message(body).empty());
    BOOST_CHECK_NE(pipeline->error().find("body encode failed"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(EncodeMessageRouteNotFoundForBytesOnlyBody) {
    // No hints anywhere and multiple routes: only() cannot disambiguate.
    auto pipeline = make_outbound_pipeline(two_routes());
    DecodedBody body;
    BOOST_CHECK(pipeline->encode_message(body).empty());
    BOOST_CHECK_NE(pipeline->error().find("failed to resolve outbound route"),
                   std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// build_protocol_pipeline_from_json
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovBuildPipeline)

BOOST_AUTO_TEST_CASE(RejectsNonObjectAndEmptyConfig) {
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json("not json", {}, &error) ==
                nullptr);
    BOOST_CHECK(build_protocol_pipeline_from_json("[]", {}, &error) == nullptr);
    BOOST_CHECK(build_protocol_pipeline_from_json("{}", {}, &error) == nullptr);
}

BOOST_AUTO_TEST_CASE(AcceptsTypedLenAndLenPrefixAliases) {
    for (const auto* type : {"typelen", "typed_len", "type_len", "typed-len",
                             "len_prefix", "id-len", "id_len"}) {
        const auto config =
            std::string(R"json({"envelope":{"type":")json") + type + "\"}}";
        std::string error;
        auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
        BOOST_CHECK_MESSAGE(pipeline != nullptr, error);
    }
}

BOOST_AUTO_TEST_CASE(RejectsUnknownEnvelopeType) {
    const auto config = R"json({"envelope":{"type":"smoke"}})json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("unknown network.protocol.envelope.type"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(NonStringEnvelopeTypeHitsJsonException) {
    const auto config = R"json({"envelope":{"type":5}})json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("invalid network.protocol"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(AcceptsBigAndLittleEndian) {
    for (const auto* endian : {"big", "little"}) {
        const auto config =
            std::string(R"json({"envelope":{"endian":")json") + endian + "\"}}";
        std::string error;
        auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
        BOOST_CHECK_MESSAGE(pipeline != nullptr, error);
    }
}

BOOST_AUTO_TEST_CASE(RejectsInvalidEndianValues) {
    {
        const auto config = R"json({"envelope":{"endian":"middle"}})json";
        std::string error;
        BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                    nullptr);
        BOOST_CHECK_NE(error.find("envelope.endian"), std::string::npos);
    }
    {
        const auto config = R"json({"envelope":{"endian":3}})json";
        std::string error;
        BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                    nullptr);
        BOOST_CHECK_NE(error.find("envelope.endian"), std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(ParsesDelimiterString) {
    const auto config =
        R"json({"envelope":{"type":"line","delimiter":"\t"}})json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK_EQUAL(pipeline->profile().envelope.delimiter, '\t');
}

BOOST_AUTO_TEST_CASE(EmptyDelimiterStringKeepsDefault) {
    const auto config =
        R"json({"envelope":{"type":"line","delimiter":""}})json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK_EQUAL(pipeline->profile().envelope.delimiter, '\n');
}

BOOST_AUTO_TEST_CASE(RejectsProviderWithIncompleteVtable) {
    FakeExtState state;
    auto no_decode = make_ext_codec(state, false, true);

    const auto config = R"json(
{
  "body": {"codec": "protobuf", "provider": "protocol.protobuf"}
}
)json";
    ProtocolBuildOptions options;
    options.external_codec_resolver =
        [&no_decode](std::string_view, std::string_view,
                     std::string*) -> const shield_protocol_codec_v1* {
        return &no_decode;
    };

    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, options, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("incomplete vtable"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(AcceptsRoutingSourceVariants) {
    for (const auto* source : {"header", "body", "none"}) {
        const auto config =
            std::string(R"json({"routing":{"source":")json") + source + "\"}}";
        std::string error;
        auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
        BOOST_CHECK_MESSAGE(pipeline != nullptr, error);
    }
}

BOOST_AUTO_TEST_CASE(RejectsInvalidRoutingSource) {
    const auto config = R"json({"routing":{"source":"sideways"}})json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("routing.source"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(AcceptsUnknownRouteForwardAction) {
    const auto config =
        R"json({"routing":{"unknown_route_action":"forward"}})json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK(pipeline->profile().unknown_route_action ==
                RouteAction::ForwardRaw);
}

BOOST_AUTO_TEST_CASE(RejectsInvalidUnknownRouteAction) {
    const auto config =
        R"json({"routing":{"unknown_route_action":"explode"}})json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("unknown_route_action"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RoutingDecodeBodyRouteFlag) {
    const auto config =
        R"json({"routing":{"decode_body_route":false,"decode_before_dispatch":true}})json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK(!pipeline->profile().decode_body_route);
    BOOST_CHECK(pipeline->profile().decode_before_dispatch);
}

BOOST_AUTO_TEST_CASE(RoutesArraySkipsNonObjectEntries) {
    const auto config = R"json(
{"routes": [5, {"id": 1, "name": "first"}]}
)json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_REQUIRE(pipeline->routes().find(1) != nullptr);
}

BOOST_AUTO_TEST_CASE(RouteDirectionsAndMetadataParse) {
    const auto config = R"json(
{
  "routes": [
    {"id": 1, "direction": "s2c"},
    {"id": 2, "direction": "bidirectional"},
    {"id": 3, "method": "do_it", "logical_service": "auth",
     "requires_auth": false, "codec_id": 6, "schema_id": 8,
     "lazy_decode": false}
  ]
}
)json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);

    const auto* first = pipeline->routes().find(1);
    BOOST_REQUIRE(first != nullptr);
    BOOST_CHECK(first->direction == RouteDirection::ServerToClient);
    const auto* second = pipeline->routes().find(2);
    BOOST_REQUIRE(second != nullptr);
    BOOST_CHECK(second->direction == RouteDirection::Bidirectional);
    const auto* third = pipeline->routes().find(3);
    BOOST_REQUIRE(third != nullptr);
    BOOST_REQUIRE(third->method_name.has_value());
    BOOST_CHECK_EQUAL(*third->method_name, "do_it");
    BOOST_REQUIRE(third->logical_service_name.has_value());
    BOOST_CHECK_EQUAL(*third->logical_service_name, "auth");
    BOOST_CHECK(!third->requires_auth);
    BOOST_CHECK_EQUAL(third->codec_id, 6u);
    BOOST_CHECK_EQUAL(third->schema_id, 8u);
    BOOST_CHECK(!third->policy.lazy_decode);
}

BOOST_AUTO_TEST_CASE(RejectsInvalidRouteAction) {
    const auto config = R"json({"routes":[{"id":1,"action":"teleport"}]})json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("routes.action"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RejectsZeroRouteId) {
    const auto config = R"json({"routes":[{"id":0}]})json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("id is required"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RejectsDuplicateRouteName) {
    const auto config = R"json(
{"routes": [{"id":1,"name":"dup"},{"id":2,"name":"dup"}]}
)json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("duplicate name"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RouteArraySkippedWhenNotArray) {
    const auto config = R"json({"routes": 7})json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, &error);
    BOOST_CHECK_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK_EQUAL(pipeline->routes().size(), 0u);
}

BOOST_AUTO_TEST_CASE(XmldefCatalogMissingFileFails) {
    const auto config = R"json(
{
  "body": {"codec": "xmldef", "catalog": "/nonexistent/cov/missing.xml"}
}
)json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("failed to load xmldef catalog"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(XmldefRejectsInvalidDefaultAction) {
    const auto config = R"json(
{
  "body": {"codec": "xmldef", "catalog": "whatever.xml"},
  "routing": {"default_action": "zoom"}
}
)json";
    std::string error;
    BOOST_CHECK(build_protocol_pipeline_from_json(config, {}, &error) ==
                nullptr);
    BOOST_CHECK_NE(error.find("default_action"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(XmldefLoadsCatalogWithRoutingDefaults) {
    const auto dir = cov_temp_dir();
    const auto catalog = dir / "cov_build_messages.xml";
    {
        std::ofstream out(catalog);
        out << "<protocol><message id=\"0x31\" "
               "name=\"built.move\"/></protocol>";
    }

    const auto config = std::string(R"json(
{
  "envelope": {"type": "idlen", "route_id_bytes": 2, "length_bytes": 2},
  "body": {"codec": "xmldef", "catalog": ")json") +
                        catalog.generic_string() + R"json("},
  "routing": {"default_action": "forward", "lazy_decode": false}
}
)json";
    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, "", 0, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    const auto* route = pipeline->routes().find(0x31);
    BOOST_REQUIRE(route != nullptr);
    BOOST_CHECK(route->policy.action == RouteAction::ForwardRaw);
    BOOST_CHECK(!route->policy.lazy_decode);
    BOOST_CHECK_EQUAL(pipeline->default_codec_name(), "xmldef");
}

// Every envelope reports its registry name; the codecs are also created
// through their default constructors here to pin the name() one-liners.
BOOST_AUTO_TEST_CASE(EnvelopeNamesAreStable) {
    using namespace shield::transport;

    LenPrefixEnvelope lenprefix;
    BOOST_CHECK_EQUAL(lenprefix.name(), "lenprefix");

    IdLenEnvelope idlen;
    BOOST_CHECK_EQUAL(idlen.name(), "idlen");

    TypeLenEnvelope typed_len;
    BOOST_CHECK_EQUAL(typed_len.name(), "typed_len");

    DelimiterEnvelope delimiter;
    BOOST_CHECK_EQUAL(delimiter.name(), "delimiter");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Round-3 additions: little-endian encode, includes-header feed framing,
// delimiter feed, raw/passthrough decode_local, xmldef direction/schema_id,
// duplicate debug names, and external-provider build errors.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LenPrefixLittleEndianEncode) {
    EnvelopeConfig config;
    config.endian = Endian::Little;
    config.length_bytes = 4;
    LenPrefixEnvelope envelope(config);

    Packet packet;
    packet.body = bytes("abc");
    const auto frame = envelope.encode(packet.ref());
    BOOST_REQUIRE_EQUAL(frame.size(), 7u);
    // Little-endian length prefix: 3, 0, 0, 0.
    BOOST_CHECK_EQUAL(frame[0], 3);
    BOOST_CHECK_EQUAL(frame[1], 0);
    BOOST_CHECK_EQUAL(frame[2], 0);
    BOOST_CHECK_EQUAL(frame[3], 0);
}

BOOST_AUTO_TEST_CASE(LenPrefixIncludesHeaderFeedsRoundTrip) {
    EnvelopeConfig config;
    config.length_bytes = 4;
    config.length_includes_header = true;
    LenPrefixEnvelope envelope(config);

    Packet packet;
    packet.body = bytes("hello");
    const auto frame = envelope.encode(packet.ref());
    BOOST_REQUIRE_EQUAL(frame.size(), 9u);

    auto packets = envelope.feed(frame.data(), frame.size());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(),
                                  packets[0].body.end(), packet.body.begin(),
                                  packet.body.end());
}

BOOST_AUTO_TEST_CASE(DelimiterFeedsMultiplePackets) {
    EnvelopeConfig config;
    config.delimiter = '\n';
    DelimiterEnvelope envelope(config);

    const std::vector<std::uint8_t> data{'a', 'b', '\n', 'c', '\n', 'd'};
    auto packets = envelope.feed(data.data(), data.size());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_EQUAL(packets.size(), 2u);
    BOOST_REQUIRE_EQUAL(packets[0].body.size(), 2u);
    BOOST_REQUIRE_EQUAL(packets[1].body.size(), 1u);
    // Trailing partial frame stays buffered.
    const std::vector<std::uint8_t> more{'e', '\n'};
    packets = envelope.feed(more.data(), more.size());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_REQUIRE(envelope.error().empty());
}

BOOST_AUTO_TEST_CASE(RawCodecDecodesToLocalBytes) {
    RawBodyCodec codec;
    Packet packet;
    packet.body = bytes("payload");
    RouteEntry route;
    route.route_id = 5;
    route.codec_id = 9;
    route.schema_id = 11;
    const DecodedBody body = codec.decode(packet.ref(), route);
    BOOST_CHECK_EQUAL(body.route_id, 5u);
    BOOST_CHECK_EQUAL(body.codec_id, 9u);
    BOOST_CHECK_EQUAL(body.schema_id, 11u);
    BOOST_CHECK_EQUAL_COLLECTIONS(body.bytes.begin(), body.bytes.end(),
                                  packet.body.begin(), packet.body.end());
}

BOOST_AUTO_TEST_CASE(PassthroughCodecDecodeLocalThrows) {
    PassthroughBodyCodec codec("msgpack");
    Packet packet;
    packet.body = bytes("x");
    RouteEntry route;
    BOOST_CHECK_THROW(codec.decode(packet.ref(), route), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(XmldefDirectionAndSchemaIdAttributes) {
    RouteTable routes;
    std::string error;
    BOOST_REQUIRE(load_xmldef_routes_from_string(
        "<message id=\"1\" name=\"up\" direction=\"c2s\" schema_id=\"7\"/>"
        "<message id=\"2\" name=\"down\" direction=\"server_to_client\"/>",
        routes, {}, &error));
    const auto* c2s = routes.find(1);
    BOOST_REQUIRE(c2s != nullptr);
    BOOST_CHECK(c2s->direction == RouteDirection::ClientToServer);
    BOOST_CHECK_EQUAL(c2s->schema_id, 7u);
    const auto* s2c = routes.find(2);
    BOOST_REQUIRE(s2c != nullptr);
    BOOST_CHECK(s2c->direction == RouteDirection::ServerToClient);
}

BOOST_AUTO_TEST_CASE(RouteTableRejectsDuplicateDebugNames) {
    RouteTable routes;
    RouteEntry first;
    first.route_id = 1;
    first.debug_name = "same.name";
    BOOST_CHECK(routes.add(std::move(first)));

    RouteEntry dup;
    dup.route_id = 2;
    dup.debug_name = "same.name";
    BOOST_CHECK(!routes.add(std::move(dup)));
}

BOOST_AUTO_TEST_CASE(BuildPipelineProviderWithoutResolverFails) {
    const auto json = nlohmann::json::parse(R"({
        "name": "p",
        "body": {"codec": "msgpack", "provider": "some.provider"}
    })");
    ProtocolBuildOptions options;  // no external_codec_resolver installed
    std::string error;
    BOOST_CHECK(
        !build_protocol_pipeline_from_json(json.dump(), options, &error));
    BOOST_CHECK_NE(error.find("no external codec resolver is available"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(BuildPipelineProviderResolverMismatchFails) {
    const auto json = nlohmann::json::parse(R"({
        "name": "p",
        "body": {"codec": "msgpack", "provider": "some.provider"}
    })");
    ProtocolBuildOptions options;
    options.external_codec_resolver =
        [](std::string_view, std::string_view,
           std::string* error) -> const shield_protocol_codec_v1* {
        if (error) *error = "provider has no such codec";
        return nullptr;
    };
    std::string error;
    BOOST_CHECK(
        !build_protocol_pipeline_from_json(json.dump(), options, &error));
    BOOST_CHECK_NE(error.find("provider has no such codec"), std::string::npos);

    // A live vtable whose codec_name does not match body.codec is refused
    // with the dedicated mismatch error.
    static shield_protocol_codec_v1 codec{};
    codec.struct_size = sizeof(shield_protocol_codec_v1);
    codec.codec_name = "other_codec";
    codec.decode = [](const shield_protocol_codec_v1*,
                      const shield_protocol_decode_args_v1*,
                      shield_protocol_decode_result_v1* out,
                      shield_error_v1*) -> int {
        if (out) {
            out->message_json = "{}";
            out->message_json_size = 2;
        }
        return 0;
    };
    codec.encode = [](const shield_protocol_codec_v1*,
                      const shield_protocol_encode_args_v1*,
                      shield_protocol_encode_result_v1* out,
                      shield_error_v1*) -> int {
        if (out) {
            out->payload = nullptr;
            out->payload_size = 0;
        }
        return 0;
    };
    ProtocolBuildOptions named;
    named.external_codec_resolver =
        [](std::string_view, std::string_view,
           std::string*) -> const shield_protocol_codec_v1* { return &codec; };
    error.clear();
    BOOST_CHECK(!build_protocol_pipeline_from_json(json.dump(), named, &error));
    BOOST_CHECK_NE(error.find("does not serve"), std::string::npos);
}
