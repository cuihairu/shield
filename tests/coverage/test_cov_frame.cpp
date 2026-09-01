#define BOOST_TEST_MODULE CovFrame
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "shield/transport/frame.hpp"

using shield::transport::Frame;
using shield::transport::FrameDecoder;
using shield::transport::FrameEncoder;
using shield::transport::FrameHeader;

namespace {

std::vector<uint8_t> bytes(std::string_view value) {
    return std::vector<uint8_t>(value.begin(), value.end());
}

std::vector<uint8_t> make_raw_frame(uint32_t length, uint16_t flags,
                                    uint16_t type, std::string_view payload) {
    std::vector<uint8_t> raw(FrameHeader::HEADER_SIZE + payload.size());
    raw[0] = static_cast<uint8_t>((length >> 24) & 0xFF);
    raw[1] = static_cast<uint8_t>((length >> 16) & 0xFF);
    raw[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
    raw[3] = static_cast<uint8_t>(length & 0xFF);
    raw[4] = static_cast<uint8_t>((flags >> 8) & 0xFF);
    raw[5] = static_cast<uint8_t>(flags & 0xFF);
    raw[6] = static_cast<uint8_t>((type >> 8) & 0xFF);
    raw[7] = static_cast<uint8_t>(type & 0xFF);
    if (!payload.empty()) {
        std::memcpy(raw.data() + FrameHeader::HEADER_SIZE, payload.data(),
                    payload.size());
    }
    return raw;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(CovFrame)

BOOST_AUTO_TEST_CASE(SerializeRoundTripsThroughParse) {
    Frame original(7, bytes("payload-data"));
    auto wire = original.serialize();
    BOOST_REQUIRE_EQUAL(wire.size(), FrameHeader::HEADER_SIZE + 12u);

    Frame parsed;
    BOOST_CHECK(parsed.parse(wire.data(), wire.size()));
    BOOST_CHECK_EQUAL(parsed.header().type, 7u);
    BOOST_CHECK_EQUAL(parsed.header().flags, 0u);
    BOOST_CHECK_EQUAL(parsed.header().length, 12u);
    BOOST_CHECK_EQUAL(parsed.total_size(), wire.size());
    auto expected = bytes("payload-data");
    BOOST_CHECK_EQUAL_COLLECTIONS(parsed.payload().begin(),
                                  parsed.payload().end(), expected.begin(),
                                  expected.end());
}

BOOST_AUTO_TEST_CASE(ParseRoundTripKeepsMutablePayload) {
    Frame original(3, bytes("abc"));
    original.mutable_payload().push_back('d');
    BOOST_CHECK_EQUAL(original.payload().size(), 4u);
    auto wire = original.serialize();
    BOOST_REQUIRE_EQUAL(wire.size(), FrameHeader::HEADER_SIZE + 4u);

    Frame parsed;
    BOOST_REQUIRE(parsed.parse(wire.data(), wire.size()));
    BOOST_CHECK_EQUAL(parsed.header().length, 3u);
    BOOST_CHECK_EQUAL(parsed.total_size(), FrameHeader::HEADER_SIZE + 3u);
    auto expected = bytes("abc");
    BOOST_CHECK_EQUAL_COLLECTIONS(parsed.payload().begin(),
                                  parsed.payload().end(), expected.begin(),
                                  expected.end());
}

BOOST_AUTO_TEST_CASE(ParseRejectsTruncatedHeader) {
    Frame parsed;
    auto wire = make_raw_frame(2, 0, 1, "ab");
    BOOST_CHECK(!parsed.parse(wire.data(), FrameHeader::HEADER_SIZE - 1));
    BOOST_CHECK(!parsed.parse(wire.data(), 0));
}

BOOST_AUTO_TEST_CASE(ParseRejectsIncompletePayload) {
    Frame parsed;
    auto wire = make_raw_frame(10, 1, 2, "ab");
    BOOST_CHECK(!parsed.parse(wire.data(), wire.size()));
    BOOST_CHECK_EQUAL(parsed.payload().size(), 0u);
}

BOOST_AUTO_TEST_CASE(ParseAcceptsEmptyPayloadFrame) {
    Frame parsed;
    auto wire = make_raw_frame(0, 0, 9, "");
    BOOST_REQUIRE_EQUAL(wire.size(), FrameHeader::HEADER_SIZE);
    BOOST_CHECK(parsed.parse(wire.data(), wire.size()));
    BOOST_CHECK_EQUAL(parsed.header().length, 0u);
    BOOST_CHECK(parsed.payload().empty());
}

BOOST_AUTO_TEST_CASE(SetMaxFrameSizeAppliesToLaterFeeds) {
    FrameEncoder encoder;
    auto wire = encoder.encode(Frame(1, bytes("hello")));

    FrameDecoder decoder;
    decoder.set_max_frame_size(3);
    auto frames = decoder.feed(wire.data(), wire.size());
    BOOST_CHECK(frames.empty());
    BOOST_CHECK_NE(decoder.error().find("frame too large"), std::string::npos);
    BOOST_CHECK_NE(decoder.error().find("max 3"), std::string::npos);

    decoder.set_max_frame_size(0);
    frames = decoder.feed(wire.data(), wire.size());
    BOOST_REQUIRE_EQUAL(frames.size(), 1u);
    BOOST_CHECK_EQUAL(frames[0].header().type, 1u);
    BOOST_CHECK(decoder.error().empty());
}

BOOST_AUTO_TEST_CASE(FeedHeaderThenPartialPayload) {
    auto wire = make_raw_frame(5, 2, 9, "hello");

    FrameDecoder decoder;
    auto frames = decoder.feed(wire.data(), FrameHeader::HEADER_SIZE + 2);
    BOOST_CHECK(frames.empty());
    BOOST_CHECK(decoder.error().empty());

    frames = decoder.feed(wire.data() + FrameHeader::HEADER_SIZE + 2, 3);
    BOOST_REQUIRE_EQUAL(frames.size(), 1u);
    BOOST_CHECK_EQUAL(frames[0].header().length, 5u);
    BOOST_CHECK_EQUAL(frames[0].header().flags, 2u);
    BOOST_CHECK_EQUAL(frames[0].header().type, 9u);
    auto expected = bytes("hello");
    BOOST_CHECK_EQUAL_COLLECTIONS(frames[0].payload().begin(),
                                  frames[0].payload().end(), expected.begin(),
                                  expected.end());
    BOOST_CHECK(decoder.error().empty());
}

BOOST_AUTO_TEST_CASE(FeedMultipleFramesInOneCall) {
    FrameEncoder encoder;
    auto first = encoder.encode(Frame(1, bytes("one")));
    auto second = encoder.encode(Frame(2, bytes("two")));
    std::vector<uint8_t> wire(first);
    wire.insert(wire.end(), second.begin(), second.end());

    FrameDecoder decoder;
    auto frames = decoder.feed(wire.data(), wire.size());
    BOOST_REQUIRE_EQUAL(frames.size(), 2u);
    BOOST_CHECK_EQUAL(frames[0].header().type, 1u);
    BOOST_CHECK_EQUAL(frames[1].header().type, 2u);
    BOOST_CHECK(decoder.error().empty());
}

BOOST_AUTO_TEST_CASE(ResetDiscardsBufferedData) {
    FrameDecoder decoder;
    const uint8_t junk[3] = {1, 2, 3};
    BOOST_CHECK(decoder.feed(junk, sizeof(junk)).empty());
    decoder.reset();

    FrameEncoder encoder;
    auto wire = encoder.encode(Frame(5, bytes("after-reset")));
    auto frames = decoder.feed(wire.data(), wire.size());
    BOOST_REQUIRE_EQUAL(frames.size(), 1u);
    BOOST_CHECK_EQUAL(frames[0].header().type, 5u);
    auto expected = bytes("after-reset");
    BOOST_CHECK_EQUAL_COLLECTIONS(frames[0].payload().begin(),
                                  frames[0].payload().end(), expected.begin(),
                                  expected.end());
    BOOST_CHECK(decoder.error().empty());
}

BOOST_AUTO_TEST_CASE(FrameEncoderDelegatesToSerialize) {
    FrameEncoder encoder;
    Frame frame(2, bytes("enc"));
    auto wire = encoder.encode(frame);
    BOOST_REQUIRE_EQUAL(wire.size(), FrameHeader::HEADER_SIZE + 3u);
    Frame back;
    BOOST_REQUIRE(back.parse(wire.data(), wire.size()));
    BOOST_CHECK_EQUAL(back.header().type, 2u);
}

BOOST_AUTO_TEST_SUITE_END()
