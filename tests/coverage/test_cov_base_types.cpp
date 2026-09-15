// shield_base unit coverage: byte_buffer / error / result / time (id has
// its own suite). Header-only types, so every case drives the public
// surface directly — including the observable edge behavior (reads past
// the end return 0 without advancing, hex_dump line breaking, remaining()
// clamping at zero).
#define BOOST_TEST_MODULE CovBaseTypes
#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "shield/base/byte_buffer.hpp"
#include "shield/base/error.hpp"
#include "shield/base/result.hpp"
#include "shield/base/time.hpp"

using shield::base::ByteBuffer;
using shield::base::Duration;
using shield::base::Error;
using shield::base::Result;

// ---------------------------------------------------------------------------
// ByteBuffer
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovByteBuffer)

BOOST_AUTO_TEST_CASE(EmptyAndCapacityCtor) {
    ByteBuffer empty;
    BOOST_CHECK(empty.empty());
    BOOST_CHECK_EQUAL(empty.size(), 0u);
    BOOST_CHECK_EQUAL(empty.read_position(), 0u);

    ByteBuffer sized(8);
    BOOST_CHECK(!sized.empty());
    BOOST_CHECK_EQUAL(sized.size(), 8u);
    // resize() value-initializes, so a capacity-constructed buffer reads
    // back as zeroes.
    for (uint8_t byte : sized.span()) {
        BOOST_CHECK_EQUAL(byte, 0u);
    }
}

BOOST_AUTO_TEST_CASE(PointerCtorAndViews) {
    const std::string raw = "shield";
    ByteBuffer buf(raw.data(), raw.size());
    BOOST_CHECK_EQUAL(buf.size(), raw.size());
    BOOST_CHECK(!buf.empty());
    BOOST_CHECK(std::equal(buf.span().begin(), buf.span().end(), raw.begin()));
    // The const view and the mutable data() alias the same bytes.
    buf.data()[0] = 'S';
    BOOST_CHECK_EQUAL(buf.span()[0], 'S');
    BOOST_CHECK(buf.capacity() >= buf.size());
}

BOOST_AUTO_TEST_CASE(WriteReadRoundTrip) {
    ByteBuffer buf;
    buf.write_uint8(0xAB);
    buf.write_uint16(0xBEEF);
    buf.write_uint32(0xDEADBEEFu);
    buf.write_uint64(0x0123456789ABCDEFULL);
    const std::string extra = "tail";
    buf.write(extra.data(), extra.size());
    BOOST_CHECK_EQUAL(buf.size(), 1u + 2u + 4u + 8u + extra.size());

    // Reads advance by the field width, in write order.
    BOOST_CHECK_EQUAL(buf.read_uint8(), 0xAB);
    BOOST_CHECK_EQUAL(buf.read_position(), 1u);
    BOOST_CHECK_EQUAL(buf.read_uint16(), 0xBEEF);
    BOOST_CHECK_EQUAL(buf.read_uint32(), 0xDEADBEEFu);
    BOOST_CHECK_EQUAL(buf.read_uint64(), 0x0123456789ABCDEFULL);
    std::string tail(buf.size() - buf.read_position(), '\0');
    std::memcpy(tail.data(), buf.data() + buf.read_position(), tail.size());
    BOOST_CHECK_EQUAL(tail, "tail");
}

BOOST_AUTO_TEST_CASE(ReadPastEndReturnsZeroWithoutAdvancing) {
    ByteBuffer buf;
    buf.write_uint8(0x7F);
    BOOST_CHECK_EQUAL(buf.read_uint8(), 0x7F);
    // Every reader guards the remaining size: exhausted reads return 0 and
    // leave the position alone (the next smaller read can still succeed).
    BOOST_CHECK_EQUAL(buf.read_uint64(), 0u);
    BOOST_CHECK_EQUAL(buf.read_position(), 1u);
    BOOST_CHECK_EQUAL(buf.read_uint8(), 0u);
    BOOST_CHECK_EQUAL(buf.read_position(), 1u);

    // A 3-byte buffer: uint32 does not fit, uint16 does.
    ByteBuffer small;
    small.write_uint8(1);
    small.write_uint8(2);
    small.write_uint8(3);
    BOOST_CHECK_EQUAL(small.read_uint32(), 0u);
    BOOST_CHECK_EQUAL(small.read_uint16(), 0x0201u);  // host order
}

BOOST_AUTO_TEST_CASE(SetReadPositionAndClear) {
    ByteBuffer buf = ByteBuffer::from_string("abcd");
    BOOST_CHECK_EQUAL(buf.read_uint8(), 'a');
    buf.set_read_position(0);
    BOOST_CHECK_EQUAL(buf.read_uint8(), 'a');
    buf.set_read_position(3);
    BOOST_CHECK_EQUAL(buf.read_uint8(), 'd');

    buf.clear();
    BOOST_CHECK(buf.empty());
    BOOST_CHECK_EQUAL(buf.read_position(), 0u);
    BOOST_CHECK_EQUAL(buf.read_uint8(), 0u);
}

BOOST_AUTO_TEST_CASE(ResizeReserve) {
    ByteBuffer buf;
    buf.reserve(64);
    BOOST_CHECK_GE(buf.capacity(), 64u);
    buf.resize(4);
    BOOST_CHECK_EQUAL(buf.size(), 4u);
    // Shrinking then growing value-initializes the grown region.
    buf.resize(8);
    for (size_t i = 4; i < 8; ++i) {
        BOOST_CHECK_EQUAL(buf.span()[i], 0u);
    }
}

BOOST_AUTO_TEST_CASE(StringRoundTrip) {
    const std::string text = "hello shield";
    ByteBuffer buf = ByteBuffer::from_string(text);
    BOOST_CHECK_EQUAL(buf.size(), text.size());
    BOOST_CHECK_EQUAL(buf.to_string(), text);
    // Embedded NULs survive the round trip (length-based, not C-string).
    const std::string binary("a\0b", 3);
    BOOST_CHECK_EQUAL(ByteBuffer::from_string(binary).to_string(), binary);
}

BOOST_AUTO_TEST_CASE(HexDumpFormatting) {
    // Short dump: lowercase hex pairs separated by spaces, no line break
    // before the 16th byte boundary. Every byte — including the last — is
    // followed by its separator, so the dump carries a trailing space.
    ByteBuffer two;
    two.write_uint8(0x00);
    two.write_uint8(0x1F);
    BOOST_CHECK_EQUAL(two.hex_dump(), "00 1f ");

    // Exactly 16 bytes: a newline lands after the last byte.
    ByteBuffer sixteen;
    for (int i = 0; i < 16; ++i) sixteen.write_uint8(0x0F);
    const std::string full = sixteen.hex_dump();
    BOOST_CHECK_EQUAL(full.back(), '\n');
    BOOST_CHECK_EQUAL(full.size(), 16u * 3u);

    // More than max_bytes: truncated with an ellipsis.
    ByteBuffer big = ByteBuffer::from_string(std::string(40, 'x'));
    const std::string clipped = big.hex_dump(16);
    BOOST_CHECK_NE(clipped.find("..."), std::string::npos);
    BOOST_CHECK_EQUAL(clipped.size(), 16u * 3u + 3u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovError)

BOOST_AUTO_TEST_CASE(DefaultIsFalsy) {
    Error err;
    BOOST_CHECK(!err);
    BOOST_CHECK(err.code().empty());
    BOOST_CHECK(err.message().empty());
    BOOST_CHECK(!err.retryable());
    BOOST_CHECK(err.detail().empty());
    BOOST_CHECK(err.context().empty());
}

BOOST_AUTO_TEST_CASE(FullConstructorAndAccessors) {
    Error err("timeout", "call timed out", true, "after 5000ms");
    BOOST_CHECK(err);
    BOOST_CHECK_EQUAL(err.code(), "timeout");
    BOOST_CHECK_EQUAL(err.message(), "call timed out");
    BOOST_CHECK(err.retryable());
    BOOST_CHECK_EQUAL(err.detail(), "after 5000ms");
}

BOOST_AUTO_TEST_CASE(FluentBuilderChain) {
    Error err = Error()
                    .with_code("not_found")
                    .with_message("service missing")
                    .with_retryable()
                    .with_detail("gateway")
                    .with_context("route", "login")
                    .with_context("node", "node-a");
    BOOST_CHECK(err);
    BOOST_CHECK_EQUAL(err.code(), "not_found");
    BOOST_CHECK_EQUAL(err.message(), "service missing");
    BOOST_CHECK(err.retryable());
    BOOST_CHECK_EQUAL(err.detail(), "gateway");
    BOOST_CHECK_EQUAL(err.context().at("route"), "login");
    BOOST_CHECK_EQUAL(err.context().at("node"), "node-a");
    BOOST_CHECK_EQUAL(err.context().size(), 2u);

    // Retryable flips both ways.
    err.with_retryable(false);
    BOOST_CHECK(!err.retryable());
}

BOOST_AUTO_TEST_CASE(CommonErrorCodes) {
    namespace e = shield::base::errors;
    BOOST_CHECK_EQUAL(std::string(e::TIMEOUT), "timeout");
    BOOST_CHECK_EQUAL(std::string(e::NOT_FOUND), "not_found");
    BOOST_CHECK_EQUAL(std::string(e::ALREADY_EXISTS), "already_exists");
    BOOST_CHECK_EQUAL(std::string(e::INVALID_ARGUMENT), "invalid_argument");
    BOOST_CHECK_EQUAL(std::string(e::PERMISSION_DENIED), "permission_denied");
    BOOST_CHECK_EQUAL(std::string(e::UNAVAILABLE), "unavailable");
    BOOST_CHECK_EQUAL(std::string(e::INTERNAL), "internal");
    BOOST_CHECK_EQUAL(std::string(e::SERVICE_NOT_FOUND), "service_not_found");
    BOOST_CHECK_EQUAL(std::string(e::SERVICE_EXITING), "service_exiting");
    BOOST_CHECK_EQUAL(std::string(e::MESSAGE_TOO_LARGE), "message_too_large");
    BOOST_CHECK_EQUAL(std::string(e::SERIALIZATION_FAILED),
                      "serialization_failed");
    BOOST_CHECK_EQUAL(std::string(e::LUA_SCRIPT_ERROR), "lua_script_error");
    BOOST_CHECK_EQUAL(std::string(e::LUA_TIMEOUT), "lua_timeout");
    BOOST_CHECK_EQUAL(std::string(e::MODULE_UNAVAILABLE), "module_unavailable");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovResult)

BOOST_AUTO_TEST_CASE(OkAndErrorStates) {
    Result<int> ok(42);
    BOOST_CHECK(ok.is_ok());
    BOOST_CHECK(!ok.is_error());
    BOOST_CHECK_EQUAL(ok.value(), 42);

    Result<int> err(Error("timeout", "expired"));
    BOOST_CHECK(err.is_error());
    BOOST_CHECK(!err.is_ok());
    BOOST_CHECK_EQUAL(err.error().code(), "timeout");
}

BOOST_AUTO_TEST_CASE(MapTransformsAndPropagates) {
    Result<int> ok(5);
    auto doubled = ok.map([](const int& v) { return v * 2; });
    BOOST_CHECK(doubled.is_ok());
    BOOST_CHECK_EQUAL(doubled.value(), 10);

    Result<int> err(Error("internal", "boom"));
    auto carried = err.map([](const int& v) { return v * 2; });
    BOOST_CHECK(carried.is_error());
    BOOST_CHECK_EQUAL(carried.error().code(), "internal");
}

BOOST_AUTO_TEST_CASE(AndThenChainsAndShortCircuits) {
    auto step = [](const int& v) -> Result<std::string> {
        if (v > 0) return Result<std::string>("positive:" + std::to_string(v));
        return Result<std::string>(Error("invalid_argument", "non-positive"));
    };

    Result<int> ok(3);
    auto chained = ok.and_then(step);
    BOOST_CHECK(chained.is_ok());
    BOOST_CHECK_EQUAL(chained.value(), "positive:3");

    Result<int> bad(-1);
    auto rejected = bad.and_then(step);
    BOOST_CHECK(rejected.is_error());
    BOOST_CHECK_EQUAL(rejected.error().code(), "invalid_argument");

    // An upstream error short-circuits without calling the step.
    Result<int> upstream(Error("unavailable", "down"));
    auto skipped = upstream.and_then(step);
    BOOST_CHECK(skipped.is_error());
    BOOST_CHECK_EQUAL(skipped.error().code(), "unavailable");
}

BOOST_AUTO_TEST_CASE(OrElseRecovers) {
    auto fallback = [](const Error& e) -> Result<int> {
        return Result<int>(7);
    };

    Result<int> err(Error("timeout", "expired"));
    auto recovered = err.or_else(fallback);
    BOOST_CHECK(recovered.is_ok());
    BOOST_CHECK_EQUAL(recovered.value(), 7);

    Result<int> ok(3);
    auto untouched = ok.or_else(fallback);
    BOOST_CHECK(untouched.is_ok());
    BOOST_CHECK_EQUAL(untouched.value(), 3);
}

BOOST_AUTO_TEST_CASE(VoidSpecialization) {
    Result<void> ok;
    BOOST_CHECK(ok.is_ok());
    BOOST_CHECK(!ok.is_error());

    Result<void> err(Error("internal", "failed"));
    BOOST_CHECK(err.is_error());
    BOOST_CHECK_EQUAL(err.error().code(), "internal");
}

BOOST_AUTO_TEST_CASE(MoveSemantics) {
    Result<std::string> ok(std::string("payload"));
    Result<std::string> moved = std::move(ok);
    BOOST_CHECK(moved.is_ok());
    BOOST_CHECK_EQUAL(moved.value(), "payload");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CovTime)

BOOST_AUTO_TEST_CASE(ClocksAreMonotonicNonDecrecreasing) {
    const auto a = shield::base::now();
    const auto b = shield::base::now();
    BOOST_CHECK(b >= a);

    const int64_t ms_a = shield::base::now_ms();
    const int64_t ms_b = shield::base::now_ms();
    BOOST_CHECK(ms_b >= ms_a);
    BOOST_CHECK_GT(ms_a, 0);
}

BOOST_AUTO_TEST_CASE(MillisConversions) {
    BOOST_CHECK_EQUAL(shield::base::to_millis(Duration(250)), 250);
    BOOST_CHECK_EQUAL(shield::base::from_millis(250), Duration(250));
    BOOST_CHECK_EQUAL(shield::base::to_millis(shield::base::from_millis(-5)),
                      -5);
}

BOOST_AUTO_TEST_CASE(ExpiryAndRemaining) {
    const auto past = shield::base::now() - Duration(1);
    const auto future = shield::base::now() + Duration(60'000);
    BOOST_CHECK(shield::base::is_expired(past));
    BOOST_CHECK(!shield::base::is_expired(future));

    // A lapsed deadline clamps to zero rather than going negative.
    BOOST_CHECK_EQUAL(shield::base::remaining(past).count(), 0);
    // A far deadline leaves (nearly) the whole budget — only the elapsed
    // microseconds of the call itself may shave off the top.
    const auto left = shield::base::remaining(future).count();
    BOOST_CHECK_LE(left, 60'000);
    BOOST_CHECK_GE(left, 59'000);
}

BOOST_AUTO_TEST_SUITE_END()
