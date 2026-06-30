#define BOOST_TEST_MODULE TransportProtocolRoutingTests
#include <boost/test/unit_test.hpp>

#include "shield/transport/protocol.hpp"

#include <cstdint>
#include <memory>
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
using shield::transport::ProtocolProfile;
using shield::transport::RawBodyCodec;
using shield::transport::create_body_codec;
using shield::transport::load_xmldef_routes_from_string;
using shield::transport::RouteAction;
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
    entry.target_service = 42;
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
    BOOST_CHECK_EQUAL(found->target_service, 42u);
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
        .target_service = 100,
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
        .target_service = 9,
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
    BOOST_CHECK_EQUAL(results[0].route->target_service, 9u);
    BOOST_CHECK_EQUAL_COLLECTIONS(results[0].packet.raw_frame.begin(),
                                  results[0].packet.raw_frame.end(),
                                  encoded.begin(), encoded.end());
}

BOOST_AUTO_TEST_CASE(ProtocolPipelineCanResolveJsonBodyRoute) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .target_service = 3,
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
}

BOOST_AUTO_TEST_CASE(BinarySchemaCodecNamesArePassthroughUntilDecoded) {
    auto protobuf = create_body_codec("protobuf");
    auto fbs = create_body_codec("fbs");
    auto sproto = create_body_codec("sproto");
    auto xmldef = create_body_codec("xmldef");

    BOOST_REQUIRE(protobuf != nullptr);
    BOOST_REQUIRE(fbs != nullptr);
    BOOST_REQUIRE(sproto != nullptr);
    BOOST_REQUIRE(xmldef != nullptr);
    BOOST_CHECK_EQUAL(protobuf->name(), "protobuf");
    BOOST_CHECK_EQUAL(fbs->name(), "fbs");
    BOOST_CHECK_EQUAL(sproto->name(), "sproto");
    BOOST_CHECK_EQUAL(xmldef->name(), "xmldef");

    Packet packet;
    packet.route_id = 88;
    packet.body = {0xde, 0xad, 0xbe, 0xef};

    RouteEntry route;
    route.route_id = 88;
    route.codec_id = 4;
    route.schema_id = 123;

    auto decoded = xmldef->decode(packet.ref(), route);
    BOOST_CHECK_EQUAL(decoded.codec_id, 4u);
    BOOST_CHECK_EQUAL(decoded.schema_id, 123u);
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.bytes.begin(), decoded.bytes.end(),
                                  packet.body.begin(), packet.body.end());
}

BOOST_AUTO_TEST_CASE(XmldefCatalogLoadsGenericRoutes) {
    RouteTable routes;
    std::string error;

    const auto xml = R"xml(
<protocol name="arena">
  <message id="0x1001" name="player.move" target_service="10"
           action="forward_raw" codec_id="4" schema_id="33"
           lazy_decode="true" />
  <route id="4098" name="auth.login" target="1"
         action="decode" schema="34" lazy_decode="false" />
</protocol>
)xml";

    BOOST_REQUIRE(load_xmldef_routes_from_string(xml, routes, {}, &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(routes.size(), 2u);

    const auto* move = routes.find(0x1001);
    BOOST_REQUIRE(move != nullptr);
    BOOST_CHECK_EQUAL(move->debug_name, "player.move");
    BOOST_CHECK_EQUAL(move->target_service, 10u);
    BOOST_CHECK_EQUAL(move->codec_id, 4u);
    BOOST_CHECK_EQUAL(move->schema_id, 33u);
    BOOST_CHECK(move->policy.action == RouteAction::ForwardRaw);
    BOOST_CHECK(move->policy.lazy_decode);

    const auto* login = routes.find_by_name("auth.login");
    BOOST_REQUIRE(login != nullptr);
    BOOST_CHECK_EQUAL(login->route_id, 4098u);
    BOOST_CHECK_EQUAL(login->target_service, 1u);
    BOOST_CHECK_EQUAL(login->schema_id, 34u);
    BOOST_CHECK(login->policy.action == RouteAction::DecodeLocal);
    BOOST_CHECK(!login->policy.lazy_decode);
}

BOOST_AUTO_TEST_SUITE_END()
