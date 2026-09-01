#define BOOST_TEST_MODULE CovCodec
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "shield/transport/codec.hpp"

using shield::transport::create_codec;
using shield::transport::JsonCodec;

namespace {

std::vector<uint8_t> bytes(std::string_view value) {
    return std::vector<uint8_t>(value.begin(), value.end());
}

}  // namespace

BOOST_AUTO_TEST_SUITE(CovCodec)

BOOST_AUTO_TEST_CASE(EncodeProducesJsonObject) {
    JsonCodec codec;
    auto data = codec.encode("ping", "body-1");
    auto text = std::string(data.begin(), data.end());
    BOOST_CHECK_NE(text.find("\"method\":\"ping\""), std::string::npos);
    BOOST_CHECK_NE(text.find("\"payload\":\"body-1\""), std::string::npos);
    BOOST_CHECK_EQUAL(codec.name(), "json");
}

BOOST_AUTO_TEST_CASE(DecodeRoundTrip) {
    JsonCodec codec;
    auto data = codec.encode("ping", "body");
    std::string method;
    std::string payload;
    BOOST_CHECK(codec.decode(data, method, payload));
    BOOST_CHECK_EQUAL(method, "ping");
    BOOST_CHECK_EQUAL(payload, "body");
}

BOOST_AUTO_TEST_CASE(DecodeMissingFieldsLeavesOutputsUnchanged) {
    JsonCodec codec;
    std::string method = "keep-method";
    std::string payload = "keep-payload";

    BOOST_CHECK(codec.decode(bytes("{\"method\":\"only\"}"), method, payload));
    BOOST_CHECK_EQUAL(method, "only");
    BOOST_CHECK_EQUAL(payload, "keep-payload");

    method = "keep-method";
    BOOST_CHECK(codec.decode(bytes("{\"payload\":\"only\"}"), method, payload));
    BOOST_CHECK_EQUAL(method, "keep-method");
    BOOST_CHECK_EQUAL(payload, "only");

    BOOST_CHECK(codec.decode(bytes("{}"), method, payload));
    BOOST_CHECK_EQUAL(method, "keep-method");
    BOOST_CHECK_EQUAL(payload, "only");
}

BOOST_AUTO_TEST_CASE(DecodeInvalidJsonReturnsFalse) {
    JsonCodec codec;
    std::string method;
    std::string payload;
    BOOST_CHECK(!codec.decode(bytes("not json"), method, payload));
    BOOST_CHECK(!codec.decode(bytes("{broken"), method, payload));
    BOOST_CHECK(method.empty());
    BOOST_CHECK(payload.empty());
}

BOOST_AUTO_TEST_CASE(FactoryCreatesJsonCodecOnlyForJson) {
    auto codec = create_codec("json");
    BOOST_REQUIRE(codec != nullptr);
    BOOST_CHECK_EQUAL(codec->name(), "json");
    auto data = codec->encode("m", "p");
    std::string method;
    std::string payload;
    BOOST_CHECK(codec->decode(data, method, payload));
    BOOST_CHECK_EQUAL(method, "m");
    BOOST_CHECK_EQUAL(payload, "p");

    BOOST_CHECK(create_codec("msgpack") == nullptr);
    BOOST_CHECK(create_codec("") == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
