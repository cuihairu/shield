#define BOOST_TEST_MODULE TransportProtocolRoutingTests
#include <boost/test/unit_test.hpp>

#include "shield/plugin/protocol_codec.h"
#include "shield/transport/protocol.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

using shield::transport::BodyCodecRegistry;
using shield::transport::Endian;
using shield::transport::EnvelopeKind;
using shield::transport::EnvelopeConfig;
using shield::transport::IdLenEnvelope;
using shield::transport::JsonBodyCodec;
using shield::transport::LenPrefixEnvelope;
using shield::transport::Packet;
using shield::transport::ProtocolPipeline;
using shield::transport::ProtocolBuildOptions;
using shield::transport::ProtocolProfile;
using shield::transport::RawBodyCodec;
using shield::transport::create_body_codec;
using shield::transport::build_protocol_pipeline_from_json;
using shield::transport::load_xmldef_routes_from_string;
using shield::transport::RouteAction;
using shield::transport::RouteDirection;
using shield::transport::RouteEntry;
using shield::transport::RoutePolicy;
using shield::transport::RouteSource;
using shield::transport::RouteTable;
using shield::transport::TypeLenEnvelope;
using shield::transport::DelimiterEnvelope;

namespace {

std::vector<std::uint8_t> bytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

char* dup_test_string(std::string_view value) {
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

std::uint8_t* dup_test_bytes(std::string_view value) {
    if (value.empty()) return nullptr;
    auto* out = static_cast<std::uint8_t*>(std::malloc(value.size()));
    if (out == nullptr) return nullptr;
    std::memcpy(out, value.data(), value.size());
    return out;
}

struct FakeProtocolCodecState {
    std::string codec_name = "protobuf";
    std::string decoded_json = R"({"uid":7,"name":"alice"})";
    std::string encoded_payload;
    std::uint32_t last_route_id = 0;
    std::uint16_t last_schema_id = 0;
    std::string last_route_name;
    std::string last_input_json;
    std::vector<std::uint8_t> last_payload;
};

int fake_protocol_decode(const shield_protocol_codec_v1* self,
                         const shield_protocol_decode_args_v1* args,
                         shield_protocol_decode_result_v1* out,
                         shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        if (err) {
            err->code = "test.invalid";
            err->message = "invalid fake decode args";
            err->phase = "test";
        }
        return -1;
    }
    auto* state = static_cast<FakeProtocolCodecState*>(self->user_data);
    state->last_route_id = args->route_id;
    state->last_schema_id = args->schema_id;
    state->last_route_name = args->route_name ? args->route_name : "";
    state->last_payload.clear();
    if (args->payload != nullptr && args->payload_size > 0) {
        state->last_payload.assign(args->payload,
                                   args->payload + args->payload_size);
    }
    out->message_json = dup_test_string(state->decoded_json);
    out->message_json_size = state->decoded_json.size();
    return out->message_json == nullptr ? -1 : 0;
}

int fake_protocol_encode(const shield_protocol_codec_v1* self,
                         const shield_protocol_encode_args_v1* args,
                         shield_protocol_encode_result_v1* out,
                         shield_error_v1* err) {
    if (self == nullptr || args == nullptr || out == nullptr ||
        self->user_data == nullptr) {
        if (err) {
            err->code = "test.invalid";
            err->message = "invalid fake encode args";
            err->phase = "test";
        }
        return -1;
    }
    auto* state = static_cast<FakeProtocolCodecState*>(self->user_data);
    state->last_route_id = args->route_id;
    state->last_schema_id = args->schema_id;
    state->last_route_name = args->route_name ? args->route_name : "";
    if (args->message_json != nullptr) {
        state->last_input_json.assign(args->message_json,
                                      args->message_json +
                                          args->message_json_size);
    } else {
        state->last_input_json.clear();
    }
    state->encoded_payload = "pb:" + state->last_input_json;
    out->payload = dup_test_bytes(state->encoded_payload);
    out->payload_size = state->encoded_payload.size();
    return out->payload == nullptr ? -1 : 0;
}

void fake_free_decode_result(const shield_protocol_codec_v1*,
                             shield_protocol_decode_result_v1* result) {
    if (result == nullptr) return;
    std::free(const_cast<char*>(result->message_json));
    result->message_json = nullptr;
    result->message_json_size = 0;
}

void fake_free_encode_result(const shield_protocol_codec_v1*,
                             shield_protocol_encode_result_v1* result) {
    if (result == nullptr) return;
    std::free(const_cast<std::uint8_t*>(result->payload));
    result->payload = nullptr;
    result->payload_size = 0;
}

shield_protocol_codec_v1 make_fake_protocol_codec(
    FakeProtocolCodecState& state) {
    shield_protocol_codec_v1 codec{};
    codec.struct_size = sizeof(shield_protocol_codec_v1);
    codec.codec_name = state.codec_name.c_str();
    codec.version = "test";
    codec.user_data = &state;
    codec.decode = fake_protocol_decode;
    codec.encode = fake_protocol_encode;
    codec.free_decode_result = fake_free_decode_result;
    codec.free_encode_result = fake_free_encode_result;
    return codec;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(TransportProtocolRouting)

BOOST_AUTO_TEST_CASE(IdLenEnvelopeExtractsRouteFromSplitFrame) {
    EnvelopeConfig config;
    config.endian = Endian::Little;
    config.route_id_bytes = 2;
    config.length_bytes = 2;
    IdLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 0x1234;
    packet.body = bytes("opaque-body");

    const auto encoded = envelope.encode(packet.ref());
    BOOST_REQUIRE(envelope.error().empty());
    BOOST_REQUIRE_GE(encoded.size(), 4u);

    auto packets = envelope.feed(encoded.data(), 3);
    BOOST_CHECK(packets.empty());
    BOOST_CHECK(envelope.error().empty());

    packets = envelope.feed(encoded.data() + 3, encoded.size() - 3);
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 0x1234u);
    BOOST_CHECK_EQUAL(packets[0].body.size(), packet.body.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(), packets[0].body.end(),
                                  packet.body.begin(), packet.body.end());
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].raw_frame.begin(),
                                  packets[0].raw_frame.end(),
                                  encoded.begin(), encoded.end());
}

BOOST_AUTO_TEST_CASE(RouteTableMapsIntegerRouteToForwardPolicy) {
    RouteTable routes;
    RouteEntry entry;
    entry.route_id = 0x2001;
    entry.direction = RouteDirection::ClientToServer;
    entry.codec_id = 7;
    entry.schema_id = 99;
    entry.debug_name = "cell.avatar.move";
    entry.policy = RoutePolicy{.action = RouteAction::ForwardRaw,
                               .lazy_decode = true};

    BOOST_CHECK(routes.add(entry));
    BOOST_CHECK(!routes.add(entry));
    BOOST_CHECK(routes.contains(0x2001));
    BOOST_CHECK_EQUAL(routes.size(), 1u);

    const auto* found = routes.find(0x2001);
    BOOST_REQUIRE(found != nullptr);
    BOOST_CHECK_EQUAL(found->codec_id, 7u);
    BOOST_CHECK_EQUAL(found->schema_id, 99u);
    BOOST_CHECK_EQUAL(found->debug_name, "cell.avatar.move");
    BOOST_CHECK(found->policy.action == RouteAction::ForwardRaw);
    BOOST_CHECK(found->policy.lazy_decode);
}

BOOST_AUTO_TEST_CASE(RouteTableRejectsDuplicateRouteNameOnAdd) {
    RouteTable routes;
    BOOST_REQUIRE(routes.add(RouteEntry{
        .route_id = 1,
        .debug_name = "login",
    }));

    BOOST_CHECK(!routes.add(RouteEntry{
        .route_id = 2,
        .debug_name = "login",
    }));
    BOOST_CHECK_EQUAL(routes.size(), 1u);
    BOOST_REQUIRE(routes.find_by_name("login") != nullptr);
    BOOST_CHECK_EQUAL(routes.find_by_name("login")->route_id, 1u);
}

BOOST_AUTO_TEST_CASE(HeaderRoutedPacketCanBeForwardedWithoutBodyDecode) {
    EnvelopeConfig config;
    config.endian = Endian::Little;
    config.route_id_bytes = 2;
    config.length_bytes = 2;
    IdLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 0x3002;
    packet.body = {0xff, 0x00, 0x10, 0x80, 0x7f};

    const auto encoded = envelope.encode(packet.ref());
    auto packets = envelope.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);

    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 0x3002,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 55,
        .schema_id = 77,
        .debug_name = "base.forward_to_cell",
        .policy = RoutePolicy{.action = RouteAction::ForwardRaw,
                              .lazy_decode = true},
    });

    const auto* route = routes.find(packets[0].route_id);
    BOOST_REQUIRE(route != nullptr);
    BOOST_REQUIRE(route->policy.action == RouteAction::ForwardRaw);

    // Forwarding can use the original frame bytes. No BodyCodec is needed.
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].raw_frame.begin(),
                                  packets[0].raw_frame.end(),
                                  encoded.begin(), encoded.end());
}

BOOST_AUTO_TEST_CASE(RawBodyCodecCopiesOnlyWhenDecodeIsRequested) {
    Packet packet;
    packet.route_id = 7;
    packet.body = bytes("decode-on-demand");

    RouteEntry route;
    route.route_id = 7;
    route.codec_id = 1;
    route.schema_id = 2;

    RawBodyCodec codec;
    auto decoded = codec.decode(packet.ref(), route);

    BOOST_CHECK_EQUAL(decoded.codec_id, 1u);
    BOOST_CHECK_EQUAL(decoded.schema_id, 2u);
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.bytes.begin(), decoded.bytes.end(),
                                  packet.body.begin(), packet.body.end());
}

BOOST_AUTO_TEST_CASE(LenPrefixEnvelopeLeavesRouteToBodyCodec) {
    LenPrefixEnvelope envelope;

    Packet packet;
    packet.body = bytes(R"({"route":"login","payload":{}})");
    const auto encoded = envelope.encode(packet.ref());
    auto packets = envelope.feed(encoded.data(), encoded.size());

    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 0u);
    BOOST_CHECK(!packets[0].ref().has_header_route());
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(), packets[0].body.end(),
                                  packet.body.begin(), packet.body.end());
}

BOOST_AUTO_TEST_CASE(IdLenEnvelopeCanUseHeaderInclusiveLength) {
    EnvelopeConfig config;
    config.endian = Endian::Big;
    config.route_id_bytes = 2;
    config.length_bytes = 2;
    config.length_includes_header = true;
    IdLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 9;
    packet.body = bytes("abc");

    const auto encoded = envelope.encode(packet.ref());
    BOOST_REQUIRE_EQUAL(encoded.size(), 7u);
    BOOST_CHECK_EQUAL(encoded[2], 0u);
    BOOST_CHECK_EQUAL(encoded[3], 7u);

    auto packets = envelope.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 9u);
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(), packets[0].body.end(),
                                  packet.body.begin(), packet.body.end());
}

BOOST_AUTO_TEST_CASE(TypeLenEnvelopeUsesTypeAsRouteId) {
    EnvelopeConfig config;
    config.endian = Endian::Big;
    config.route_id_bytes = 1;
    config.length_bytes = 2;
    TypeLenEnvelope envelope(config);

    Packet packet;
    packet.route_id = 0x7f;
    packet.body = bytes("typed");

    const auto encoded = envelope.encode(packet.ref());
    auto packets = envelope.feed(encoded.data(), encoded.size());

    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    BOOST_CHECK_EQUAL(packets[0].route_id, 0x7fu);
    BOOST_CHECK_EQUAL(packets[0].kind, 0x7fu);
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(), packets[0].body.end(),
                                  packet.body.begin(), packet.body.end());
}

BOOST_AUTO_TEST_CASE(DelimiterEnvelopeSplitsLineFrames) {
    DelimiterEnvelope envelope;
    const auto input = bytes("one\ntwo\npartial");

    auto packets = envelope.feed(input.data(), input.size());
    BOOST_REQUIRE_EQUAL(packets.size(), 2u);

    const auto one = bytes("one");
    const auto two = bytes("two");
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(), packets[0].body.end(),
                                  one.begin(), one.end());
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[1].body.begin(), packets[1].body.end(),
                                  two.begin(), two.end());

    const auto tail = bytes("-done\n");
    packets = envelope.feed(tail.data(), tail.size());
    BOOST_REQUIRE_EQUAL(packets.size(), 1u);
    const auto partial = bytes("partial-done");
    BOOST_CHECK_EQUAL_COLLECTIONS(packets[0].body.begin(), packets[0].body.end(),
                                  partial.begin(), partial.end());
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineForwardsHeaderRouteWithoutDecode) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 0x11,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .schema_id = 100,
        .debug_name = "cell.forward",
        .policy = RoutePolicy{.action = RouteAction::ForwardRaw,
                              .lazy_decode = true},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<RawBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::IdLen;
    profile.envelope.endian = Endian::Little;
    profile.envelope.route_id_bytes = 2;
    profile.envelope.length_bytes = 2;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Header;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.route_id = 0x11;
    packet.body = {0x01, 0x02, 0x03};
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE(pipeline.error().empty());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_CHECK(results[0].should_forward_raw());
    BOOST_CHECK(!results[0].decoded());
    BOOST_REQUIRE(results[0].route != nullptr);
    BOOST_CHECK_EQUAL(results[0].route->route_id, 0x2001u);
    BOOST_CHECK_EQUAL_COLLECTIONS(results[0].packet.raw_frame.begin(),
                                  results[0].packet.raw_frame.end(),
                                  encoded.begin(), encoded.end());
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineCanResolveJsonBodyRoute) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .schema_id = 0,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = false},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    profile.decode_before_dispatch = false;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.body = bytes(R"({"route":"login","payload":{"uid":1}})");
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE(pipeline.error().empty());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_REQUIRE(results[0].route != nullptr);
    BOOST_CHECK_EQUAL(results[0].packet.route_id, 1001u);
    BOOST_CHECK_EQUAL(results[0].route->debug_name, "login");
    BOOST_CHECK(results[0].decoded());
    BOOST_CHECK_EQUAL(results[0].decoded_body->route_name, "login");
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_REQUIRE(results[0].decoded_body->message->is_object());
    BOOST_CHECK_EQUAL((*results[0].decoded_body->message)["uid"].get<int>(), 1);
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineDropsUnknownRouteWithoutError) {
    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<RawBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::IdLen;
    profile.envelope.endian = Endian::Little;
    profile.envelope.route_id_bytes = 2;
    profile.envelope.length_bytes = 2;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Header;
    profile.unknown_route_action = RouteAction::Drop;

    RouteTable routes;
    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.route_id = 0x77;
    packet.body = bytes("opaque");
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE(pipeline.error().empty());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_CHECK(results[0].should_drop());
    BOOST_CHECK(results[0].route == nullptr);
}

BOOST_AUTO_TEST_CASE(BuildProtocolPipelineUsesListenerMaxFrameSizeFallback) {
    const auto config = R"json(
{
  "name": "json.simple",
  "envelope": {
    "type": "lenprefix",
    "length_bytes": 4
  },
  "body": {
    "codec": "json"
  }
}
)json";

    std::string error;
    auto pipeline =
        build_protocol_pipeline_from_json(config, "", 64, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK_EQUAL(pipeline->profile().envelope.max_frame_size, 64u);
}

BOOST_AUTO_TEST_CASE(BuildProtocolPipelineUsesExternalBodyCodecProvider) {
    FakeProtocolCodecState state;
    auto fake_codec = make_fake_protocol_codec(state);

    const auto config = R"json(
{
  "name": "game.protobuf",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "protobuf",
    "provider": "protocol.protobuf"
  },
  "routes": [
    {
      "id": 4097,
      "name": "game.Login",
      "direction": "c2s",
      "requires_auth": false,
      "codec_id": 1,
      "schema_id": 42,
      "action": "decode",
      "lazy_decode": false
    }
  ]
}
)json";

    ProtocolBuildOptions options;
    options.external_codec_resolver =
        [&](std::string_view provider, std::string_view codec_name,
            std::string*) -> const shield_protocol_codec_v1* {
        BOOST_CHECK(provider == "protocol.protobuf");
        BOOST_CHECK(codec_name == "protobuf");
        return &fake_codec;
    };

    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, options, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK_EQUAL(std::string(pipeline->default_codec_name()), "protobuf");

    Packet packet;
    packet.route_id = 4097;
    packet.body = bytes("wire-protobuf");
    const auto encoded = pipeline->encode(packet.ref());
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());

    auto results = pipeline->feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_REQUIRE(results[0].decoded());
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_CHECK_EQUAL((*results[0].decoded_body->message)["uid"].get<int>(), 7);
    BOOST_CHECK_EQUAL(
        (*results[0].decoded_body->message)["name"].get<std::string>(),
        "alice");
    BOOST_CHECK_EQUAL(state.last_route_id, 4097u);
    BOOST_CHECK_EQUAL(state.last_schema_id, 42u);
    BOOST_CHECK_EQUAL(state.last_route_name, "game.Login");
    BOOST_CHECK_EQUAL_COLLECTIONS(state.last_payload.begin(),
                                  state.last_payload.end(),
                                  packet.body.begin(), packet.body.end());

    shield::transport::DecodedBody outbound;
    outbound.route_id = 4097;
    outbound.message =
        nlohmann::json::object({{"uid", 9}, {"name", "bob"}});
    const auto outbound_frame = pipeline->encode_message(outbound);
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());
    BOOST_REQUIRE_GE(outbound_frame.size(), 4u);
    BOOST_CHECK_EQUAL(state.last_route_id, 4097u);
    BOOST_CHECK_EQUAL(state.last_schema_id, 42u);
    BOOST_CHECK_EQUAL(state.last_route_name, "game.Login");

    const auto outbound_json = nlohmann::json::parse(state.last_input_json);
    BOOST_CHECK_EQUAL(outbound_json["uid"].get<int>(), 9);
    BOOST_CHECK_EQUAL(outbound_json["name"].get<std::string>(), "bob");

    const auto expected_payload = bytes(state.encoded_payload);
    BOOST_CHECK_EQUAL_COLLECTIONS(outbound_frame.begin() + 4,
                                  outbound_frame.end(),
                                  expected_payload.begin(),
                                  expected_payload.end());
}

BOOST_AUTO_TEST_CASE(BuildProtocolPipelineUsesMsgpackExternalProvider) {
    FakeProtocolCodecState state;
    state.codec_name = "msgpack";
    state.decoded_json = R"({"ok":true})";
    auto fake_codec = make_fake_protocol_codec(state);

    const auto config = R"json(
{
  "name": "game.msgpack",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "msgpack",
    "provider": "protocol.msgpack"
  },
  "routes": [
    {
      "id": 4098,
      "name": "game.Ping",
      "action": "decode",
      "lazy_decode": false
    }
  ]
}
)json";

    ProtocolBuildOptions options;
    options.external_codec_resolver =
        [&](std::string_view provider, std::string_view codec_name,
            std::string*) -> const shield_protocol_codec_v1* {
        BOOST_CHECK(provider == "protocol.msgpack");
        BOOST_CHECK(codec_name == "msgpack");
        return &fake_codec;
    };

    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, options, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_CHECK_EQUAL(std::string(pipeline->default_codec_name()), "msgpack");

    Packet packet;
    packet.route_id = 4098;
    packet.body = bytes("wire-msgpack");
    const auto encoded = pipeline->encode(packet.ref());
    BOOST_REQUIRE_MESSAGE(pipeline->error().empty(), pipeline->error());

    auto results = pipeline->feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_REQUIRE(results[0].ok());
    BOOST_REQUIRE(results[0].decoded());
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_CHECK_EQUAL((*results[0].decoded_body->message)["ok"].get<bool>(),
                      true);
    BOOST_CHECK_EQUAL(state.last_route_id, 4098u);
    BOOST_CHECK_EQUAL(state.last_route_name, "game.Ping");
}

BOOST_AUTO_TEST_CASE(BuildProtocolPipelinePropagatesMissingExternalProvider) {
    const auto config = R"json(
{
  "name": "game.protobuf",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "protobuf",
    "provider": "protocol.protobuf"
  }
}
)json";

    ProtocolBuildOptions options;
    options.external_codec_resolver =
        [](std::string_view, std::string_view,
           std::string* resolver_error) -> const shield_protocol_codec_v1* {
        if (resolver_error) {
            *resolver_error = "missing protocol.protobuf provider";
        }
        return nullptr;
    };

    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, options, &error);
    BOOST_CHECK(pipeline == nullptr);
    BOOST_CHECK_NE(error.find("missing protocol.protobuf provider"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(BuildProtocolPipelineRejectsProviderWithoutResolver) {
    const auto config = R"json(
{
  "name": "game.protobuf",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "protobuf",
    "provider": "protocol.protobuf"
  }
}
)json";

    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, {}, 0, &error);
    BOOST_CHECK(pipeline == nullptr);
    BOOST_CHECK_NE(error.find("no external codec resolver"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(BuildProtocolPipelineRejectsProviderCodecMismatch) {
    FakeProtocolCodecState state;
    state.codec_name = "sproto";
    auto fake_codec = make_fake_protocol_codec(state);

    const auto config = R"json(
{
  "name": "game.protobuf",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "protobuf",
    "provider": "protocol.protobuf"
  }
}
)json";

    ProtocolBuildOptions options;
    options.external_codec_resolver =
        [&](std::string_view, std::string_view,
            std::string*) -> const shield_protocol_codec_v1* {
        return &fake_codec;
    };

    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(config, options, &error);
    BOOST_CHECK(pipeline == nullptr);
    BOOST_CHECK_NE(error.find("provider does not serve"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(BuildProtocolPipelineAcceptsForwardAliasForXmldefDefaultAction) {
    const auto temp_dir = std::filesystem::temp_directory_path() /
                          "shield_protocol_routing_tests";
    std::filesystem::create_directories(temp_dir);
    const auto catalog_path = temp_dir / "messages.xml";
    {
        std::ofstream out(catalog_path);
        out << R"xml(<protocol name="arena">
  <message id="0x1001" name="player.move" />
</protocol>)xml";
    }

    const auto config = std::string(R"json(
{
  "name": "xmldef.default",
  "envelope": {
    "type": "idlen",
    "route_id_bytes": 2,
    "length_bytes": 2
  },
  "body": {
    "codec": "xmldef",
    "catalog": "messages.xml"
  },
  "routing": {
    "default_action": "forward"
  }
}
)json");

    std::string error;
    auto pipeline = build_protocol_pipeline_from_json(
        config, temp_dir.string(), 0, &error);
    BOOST_REQUIRE_MESSAGE(pipeline != nullptr, error);
    BOOST_REQUIRE(pipeline->routes().find(0x1001) != nullptr);
    BOOST_CHECK(pipeline->routes().find(0x1001)->policy.action ==
                RouteAction::ForwardRaw);
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineUsesProfileCodecForDecodeLocalRoutes) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 9,
        .schema_id = 42,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = false},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.body = bytes(R"({"route":"login","payload":{"uid":1}})");
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE(pipeline.error().empty());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_REQUIRE(results[0].route != nullptr);
    BOOST_CHECK(results[0].decoded());
    BOOST_CHECK_EQUAL(results[0].decoded_body->codec_id, 9u);
    BOOST_CHECK_EQUAL(results[0].decoded_body->schema_id, 42u);
    BOOST_CHECK_EQUAL(results[0].decoded_body->route_name, "login");
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineCanMaterializeLazyDecodeLocalResult) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 9,
        .schema_id = 42,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = true},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    profile.decode_before_dispatch = false;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.body = bytes(R"({"route":"login","payload":{"uid":1}})");
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE(pipeline.error().empty());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_REQUIRE(results[0].route != nullptr);
    BOOST_CHECK(!results[0].decoded());

    BOOST_REQUIRE(pipeline.materialize_decode(results[0]));
    BOOST_CHECK(results[0].decoded());
    BOOST_CHECK_EQUAL(results[0].decoded_body->codec_id, 9u);
    BOOST_CHECK_EQUAL(results[0].decoded_body->schema_id, 42u);
    BOOST_CHECK_EQUAL(results[0].decoded_body->route_name, "login");
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_REQUIRE(results[0].decoded_body->message->is_object());
    BOOST_CHECK_EQUAL((*results[0].decoded_body->message)["uid"].get<int>(), 1);
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineCanEncodeAndDecodeJsonBusinessMessage) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .schema_id = 0,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = false},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    shield::transport::DecodedBody body;
    body.message = nlohmann::json::object({{"uid", 7}});

    const auto encoded = pipeline.encode_message(std::move(body));
    BOOST_REQUIRE(!encoded.empty());
    BOOST_REQUIRE(pipeline.error().empty());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_REQUIRE(results[0].ok());
    BOOST_REQUIRE(results[0].route != nullptr);
    BOOST_CHECK_EQUAL(results[0].route->route_id, 1001u);
    BOOST_CHECK(results[0].decoded());
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_CHECK_EQUAL(results[0].decoded_body->route_name, "login");
    BOOST_CHECK_EQUAL((*results[0].decoded_body->message)["uid"].get<int>(), 7);
}

BOOST_AUTO_TEST_CASE(StructuredCodecsRejectRawByteEgress) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .schema_id = 0,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = false},
    });

    {
        BodyCodecRegistry codecs;
        BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

        ProtocolProfile profile;
        profile.envelope_kind = EnvelopeKind::LenPrefix;
        profile.default_codec_id = 1;
        profile.route_source = RouteSource::Body;
        profile.decode_body_route = true;

        ProtocolPipeline pipeline(profile, routes, std::move(codecs));

        shield::transport::DecodedBody body;
        body.bytes = bytes("raw");

        const auto encoded = pipeline.encode_message(body);
        BOOST_CHECK(encoded.empty());
        BOOST_CHECK_NE(pipeline.error().find("expects business message"),
                       std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(BinarySchemaCodecNamesCannotDecodeLocalWithoutImplementation) {
    auto protobuf = create_body_codec("protobuf");
    auto fbs = create_body_codec("fbs");
    auto sproto = create_body_codec("sproto");
    auto xmldef = create_body_codec("xmldef");
    auto msgpack = create_body_codec("msgpack");

    BOOST_REQUIRE(protobuf != nullptr);
    BOOST_REQUIRE(fbs != nullptr);
    BOOST_REQUIRE(sproto != nullptr);
    BOOST_REQUIRE(xmldef != nullptr);
    BOOST_REQUIRE(msgpack != nullptr);
    BOOST_CHECK_EQUAL(protobuf->name(), "protobuf");
    BOOST_CHECK_EQUAL(fbs->name(), "fbs");
    BOOST_CHECK_EQUAL(sproto->name(), "sproto");
    BOOST_CHECK_EQUAL(xmldef->name(), "xmldef");
    BOOST_CHECK_EQUAL(msgpack->name(), "msgpack");

    Packet packet;
    packet.route_id = 88;
    packet.body = {0xde, 0xad, 0xbe, 0xef};

    RouteEntry route;
    route.route_id = 88;
    route.codec_id = 4;
    route.schema_id = 123;

    BOOST_CHECK_THROW(xmldef->decode(packet.ref(), route), std::runtime_error);
    BOOST_CHECK_THROW(protobuf->decode(packet.ref(), route), std::runtime_error);
    BOOST_CHECK_THROW(fbs->decode(packet.ref(), route), std::runtime_error);
    BOOST_CHECK_THROW(sproto->decode(packet.ref(), route), std::runtime_error);
    BOOST_CHECK_THROW(msgpack->decode(packet.ref(), route), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineRejectsDecodeLocalForPlaceholderCodec) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 0x1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .schema_id = 0,
        .debug_name = "player.move",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = false},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, create_body_codec("xmldef")));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::IdLen;
    profile.envelope.endian = Endian::Little;
    profile.envelope.route_id_bytes = 2;
    profile.envelope.length_bytes = 2;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Header;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.route_id = 0x1001;
    packet.body = bytes("opaque");
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE(pipeline.error().empty());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(!results[0].ok());
    BOOST_CHECK_NE(results[0].error.find("does not support decode_local"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(XmldefCatalogLoadsGenericRoutes) {
    RouteTable routes;
    std::string error;

    const auto xml = R"xml(
<protocol name="arena">
  <message id="0x1001" name="player.move" direction="c2s"
           action="forward_raw" codec_id="4" schema_id="33"
           lazy_decode="true" requires_auth="false" />
  <route id="4098" name="auth.login" direction="c2s"
         action="decode" schema="34" lazy_decode="false" />
</protocol>
)xml";

    BOOST_REQUIRE(load_xmldef_routes_from_string(xml, routes, {}, &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(routes.size(), 2u);

    const auto* move = routes.find(0x1001);
    BOOST_REQUIRE(move != nullptr);
    BOOST_CHECK_EQUAL(move->debug_name, "player.move");
    BOOST_CHECK(move->direction == RouteDirection::ClientToServer);
    BOOST_CHECK(!move->requires_auth);
    BOOST_CHECK_EQUAL(move->codec_id, 4u);
    BOOST_CHECK_EQUAL(move->schema_id, 33u);
    BOOST_CHECK(move->policy.action == RouteAction::ForwardRaw);
    BOOST_CHECK(move->policy.lazy_decode);

    const auto* login = routes.find_by_name("auth.login");
    BOOST_REQUIRE(login != nullptr);
    BOOST_CHECK_EQUAL(login->route_id, 4098u);
    BOOST_CHECK(login->direction == RouteDirection::ClientToServer);
    BOOST_CHECK_EQUAL(login->schema_id, 34u);
    BOOST_CHECK(login->policy.action == RouteAction::DecodeLocal);
    BOOST_CHECK(!login->policy.lazy_decode);
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineDecodesTypeLenHeaderRouteWithJsonCodec) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 0x2001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .debug_name = "battle.attack",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = false},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::TypeLen;
    profile.envelope.endian = Endian::Big;
    profile.envelope.route_id_bytes = 2;
    profile.envelope.length_bytes = 2;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Header;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.route_id = 0x2001;
    packet.body = bytes(R"({"target":7,"damage":42})");
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE_MESSAGE(pipeline.error().empty(), pipeline.error());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_REQUIRE(results[0].route != nullptr);
    BOOST_CHECK_EQUAL(results[0].route->route_id, 0x2001u);
    BOOST_CHECK_EQUAL(results[0].route->debug_name, "battle.attack");
    BOOST_CHECK(results[0].decoded());
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_CHECK_EQUAL(
        (*results[0].decoded_body->message)["damage"].get<int>(), 42);
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineResolvesBodyRouteWithDelimiterEnvelope) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = false},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::Delimiter;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    const auto json_line =
        nlohmann::json::object({{"route", "login"}, {"payload", {{"uid", 1}}}})
            .dump();
    auto feed_data = bytes(json_line);
    feed_data.push_back('\n');

    auto results = pipeline.feed(feed_data.data(), feed_data.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_REQUIRE(results[0].route != nullptr);
    BOOST_CHECK_EQUAL(results[0].packet.route_id, 1001u);
    BOOST_CHECK_EQUAL(results[0].route->debug_name, "login");
    BOOST_CHECK(results[0].decoded());
    BOOST_CHECK_EQUAL(results[0].decoded_body->route_name, "login");
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_CHECK_EQUAL((*results[0].decoded_body->message)["uid"].get<int>(),
                      1);
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineDecodeBeforeDispatchOverridesLazyDecode) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = true},
    });

    BodyCodecRegistry codecs;
    BOOST_REQUIRE(codecs.add(1, std::make_unique<JsonBodyCodec>()));

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    profile.decode_before_dispatch = true;

    ProtocolPipeline pipeline(profile, std::move(routes), std::move(codecs));

    Packet packet;
    packet.body = bytes(R"({"route":"login","payload":{"x":1}})");
    const auto encoded = pipeline.encode(packet.ref());
    BOOST_REQUIRE_MESSAGE(pipeline.error().empty(), pipeline.error());

    auto results = pipeline.feed(encoded.data(), encoded.size());
    BOOST_REQUIRE_EQUAL(results.size(), 1u);
    BOOST_CHECK(results[0].ok());
    BOOST_CHECK(results[0].decoded());
    BOOST_REQUIRE(results[0].decoded_body->has_message());
    BOOST_CHECK_EQUAL(
        (*results[0].decoded_body->message)["x"].get<int>(), 1);
}

BOOST_AUTO_TEST_SUITE_END()
