#define BOOST_TEST_MODULE CovBaseId
#include <boost/test/unit_test.hpp>
#include <set>
#include <string>

#include "shield/base/id.hpp"

using shield::base::NodeId;
using shield::base::ServiceId;
using shield::base::TraceId;

BOOST_AUTO_TEST_SUITE(CovBaseId)

BOOST_AUTO_TEST_CASE(ServiceIdGenerateIsUniqueAndValid) {
    auto a = ServiceId::generate();
    auto b = ServiceId::generate();
    BOOST_CHECK(a.is_valid());
    BOOST_CHECK(b.is_valid());
    BOOST_CHECK(a);
    BOOST_CHECK_NE(a.value(), b.value());
    BOOST_CHECK(a != b);
    BOOST_CHECK_EQUAL(a.to_string(), "svc:" + std::to_string(a.value()));
}

BOOST_AUTO_TEST_CASE(ServiceIdDefaultIsInvalid) {
    ServiceId id;
    BOOST_CHECK(!id.is_valid());
    BOOST_CHECK(!id);
    BOOST_CHECK(id == ServiceId(0));
    BOOST_CHECK(id != ServiceId(1));
}

BOOST_AUTO_TEST_CASE(TraceIdGenerateIsValidAndUnique) {
    auto a = TraceId::generate();
    auto b = TraceId::generate();
    BOOST_CHECK(a.is_valid());
    BOOST_CHECK(a);
    BOOST_CHECK_NE(a.value(), b.value());
    BOOST_CHECK(a != b);
    BOOST_CHECK_EQUAL(a.to_string().size(), 6u + 16u);
    BOOST_CHECK_EQUAL(a.to_string().substr(0, 6), "trace:");
    BOOST_CHECK_EQUAL(TraceId(0).to_string(), "trace:none");
}

BOOST_AUTO_TEST_CASE(TraceIdFromString) {
    BOOST_CHECK_EQUAL(TraceId::from_string("trace:00000000000000ff").value(),
                      0xffULL);
    BOOST_CHECK_EQUAL(TraceId::from_string("deadbeef").value(), 0xdeadbeefULL);
    BOOST_CHECK_EQUAL(TraceId::from_string("trace:zz-not-hex").value(), 0ULL);
    BOOST_CHECK_EQUAL(TraceId::from_string("").value(), 0ULL);
    BOOST_CHECK_EQUAL(TraceId::from_string("trace:0x1234").value(), 0x1234ULL);
    BOOST_CHECK(TraceId(1) == TraceId::from_string("trace:0000000000000001"));
    BOOST_CHECK(TraceId(1) != TraceId(2));
}

BOOST_AUTO_TEST_CASE(NodeIdLocalAndFromString) {
    auto local = NodeId::local();
    BOOST_CHECK(local.is_valid());
    BOOST_CHECK(local);
    BOOST_CHECK_EQUAL(local.value(), "node-local");
    BOOST_CHECK_EQUAL(local.to_string(), "node-local");

    auto parsed = NodeId::from_string("node-a");
    BOOST_CHECK(parsed.is_valid());
    BOOST_CHECK_EQUAL(parsed.value(), "node-a");
    BOOST_CHECK(parsed == NodeId("node-a"));
    BOOST_CHECK(NodeId() == NodeId(""));
    BOOST_CHECK(NodeId("a") != NodeId("b"));
}

// ---------------------------------------------------------------------------
// Branch-coverage additions (purely additive).
// ---------------------------------------------------------------------------

// The Mersenne-Twister behind TraceId::generate() only refreshes its state
// block every 312 draws; generating past that boundary exercises the refill
// branches inside the inlined engine.
BOOST_AUTO_TEST_CASE(TraceIdGenerateAcrossTwisterRefills) {
    std::set<uint64_t> seen;
    for (int i = 0; i < 1200; ++i) {
        seen.insert(TraceId::generate().value());
    }
    // With 48 random bits per id, collisions are astronomically unlikely.
    BOOST_CHECK_GT(seen.size(), 1000u);
}

BOOST_AUTO_TEST_CASE(ServiceIdGenerateAcrossCounterWindow) {
    std::set<uint64_t> seen;
    for (int i = 0; i < 500; ++i) {
        seen.insert(ServiceId::generate().value());
    }
    BOOST_CHECK_EQUAL(seen.size(), 500u);
}

BOOST_AUTO_TEST_CASE(TraceIdRoundTripThroughString) {
    for (int i = 0; i < 50; ++i) {
        const auto id = TraceId::generate();
        const auto parsed = TraceId::from_string(id.to_string());
        BOOST_CHECK_EQUAL(parsed.value(), id.value());
        BOOST_CHECK(parsed == id);
    }
    // A plain "trace:"-less hex string also round-trips.
    const TraceId made(0x0123456789abcdefULL);
    BOOST_CHECK(TraceId::from_string(made.to_string()) == made);
}

BOOST_AUTO_TEST_SUITE_END()
