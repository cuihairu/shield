#define BOOST_TEST_MODULE TransportFrameTests
#include <boost/test/unit_test.hpp>

#include "shield/transport/frame.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using shield::transport::Frame;
using shield::transport::FrameDecoder;
using shield::transport::FrameEncoder;

namespace {

std::vector<uint8_t> bytes(std::string_view value) {
    return std::vector<uint8_t>(value.begin(), value.end());
}

}  // namespace

BOOST_AUTO_TEST_SUITE(TransportFrame)

BOOST_AUTO_TEST_CASE(FrameDecoderHandlesSplitFrames) {
    FrameEncoder encoder;
    auto encoded = encoder.encode(Frame(1, bytes("hello")));

    FrameDecoder decoder;
    auto frames = decoder.feed(encoded.data(), 3);
    BOOST_CHECK(frames.empty());
    BOOST_CHECK(decoder.error().empty());

    frames = decoder.feed(encoded.data() + 3, encoded.size() - 3);
    BOOST_REQUIRE_EQUAL(frames.size(), 1u);
    BOOST_CHECK_EQUAL(frames[0].header().type, 1u);
    auto expected = bytes("hello");
    BOOST_CHECK_EQUAL_COLLECTIONS(frames[0].payload().begin(),
                                  frames[0].payload().end(),
                                  expected.begin(), expected.end());
    BOOST_CHECK(decoder.error().empty());
}

BOOST_AUTO_TEST_CASE(FrameDecoderRejectsOversizePayload) {
    FrameEncoder encoder;
    auto encoded = encoder.encode(Frame(2, bytes("too-large")));

    FrameDecoder decoder(4);
    auto frames = decoder.feed(encoded.data(), encoded.size());

    BOOST_CHECK(frames.empty());
    BOOST_CHECK_NE(decoder.error().find("frame too large"), std::string::npos);
    BOOST_CHECK_NE(decoder.error().find("max 4"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(FrameDecoderCanRecoverAfterOversizePayload) {
    FrameEncoder encoder;
    auto large = encoder.encode(Frame(3, bytes("too-large")));
    auto small = encoder.encode(Frame(4, bytes("ok")));

    FrameDecoder decoder(4);
    auto frames = decoder.feed(large.data(), large.size());
    BOOST_CHECK(frames.empty());
    BOOST_CHECK(!decoder.error().empty());

    frames = decoder.feed(small.data(), small.size());
    BOOST_REQUIRE_EQUAL(frames.size(), 1u);
    BOOST_CHECK_EQUAL(frames[0].header().type, 4u);
    auto expected = bytes("ok");
    BOOST_CHECK_EQUAL_COLLECTIONS(frames[0].payload().begin(),
                                  frames[0].payload().end(),
                                  expected.begin(), expected.end());
    BOOST_CHECK(decoder.error().empty());
}

BOOST_AUTO_TEST_SUITE_END()
