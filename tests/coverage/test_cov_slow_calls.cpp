// SlowCallRing coverage: pure-logic drive of the process-wide slow-call
// ring (Phase B of the /ops/profile plan) against stack-local instances —
// the singleton stays untouched here so these cases cannot pollute (nor
// be polluted by) the call paths other suites exercise.
#define BOOST_TEST_MODULE CovSlowCalls
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <thread>

#include "shield/lua/slow_calls.hpp"

namespace {

uint64_t steady_ms_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// A real sleep is the only way to age a span past the threshold: the
// ring takes its own completion timestamp, so tests cannot inject one.
void age(uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

BOOST_AUTO_TEST_SUITE(SlowCallRingTests)

// Gate closed: nothing records, and enable(0) is the "off" sentinel.
BOOST_AUTO_TEST_CASE(GateClosedRecordsNothing) {
    shield::lua::SlowCallRing ring;
    BOOST_CHECK(!ring.enabled());

    ring.maybe_record(steady_ms_now() - 5000, "caller", "callee", true);
    auto snap = ring.snapshot();
    BOOST_CHECK(snap.recent.empty());
    BOOST_CHECK_EQUAL(snap.total_recorded, 0U);

    ring.enable(0);  // zero stays off — the gate never flaps
    BOOST_CHECK(!ring.enabled());

    // begin_ms == 0 means "suspended before the gate went up": dropped
    // even with the gate armed.
    ring.enable(10);
    BOOST_CHECK(ring.enabled());
    ring.maybe_record(0, "caller", "callee", true);
    BOOST_CHECK_EQUAL(ring.snapshot().total_recorded, 0U);
}

// Sub-threshold spans drop without recording.
BOOST_AUTO_TEST_CASE(SubThresholdSpanDropped) {
    shield::lua::SlowCallRing ring;
    ring.enable(500);
    const uint64_t begin = steady_ms_now();
    ring.maybe_record(begin, "caller", "callee", true);  // ~0ms < 500
    BOOST_CHECK_EQUAL(ring.snapshot().total_recorded, 0U);
}

// A span past the threshold lands with every field intact; the boundary
// is inclusive (>= threshold records — aged 12ms against a 10ms floor,
// the recorded span must clear the threshold and land in the ring).
BOOST_AUTO_TEST_CASE(AboveThresholdRecordedWithFields) {
    shield::lua::SlowCallRing ring;
    ring.enable(10);
    const uint64_t begin = steady_ms_now();  // suspend starts the meter
    age(12);                                 // the call spends 12ms in flight
    ring.maybe_record(begin, "svc_a", "svc_b", true);
    auto snap = ring.snapshot();
    BOOST_REQUIRE_EQUAL(snap.recent.size(), 1U);
    BOOST_CHECK_EQUAL(snap.total_recorded, 1U);
    const auto& rec = snap.recent[0];
    BOOST_CHECK_EQUAL(rec.caller, "svc_a");
    BOOST_CHECK_EQUAL(rec.callee, "svc_b");
    BOOST_CHECK_GE(rec.elapsed_ms, 10U);
    BOOST_CHECK(rec.ok);
    BOOST_CHECK_GT(rec.at, 0);  // wall-clock seconds stamped
}

// A failed (non-timeout) completion records with ok == false.
BOOST_AUTO_TEST_CASE(FailedCompletionRecordsOkFalse) {
    shield::lua::SlowCallRing ring;
    ring.enable(1);
    const uint64_t begin = steady_ms_now();
    age(3);  // the failed span still ages past the threshold
    ring.maybe_record(begin, "caller", "callee", false);
    auto snap = ring.snapshot();
    BOOST_REQUIRE_EQUAL(snap.recent.size(), 1U);
    BOOST_CHECK(!snap.recent[0].ok);
}

// Newest first: independent instances, strict reverse order.
BOOST_AUTO_TEST_CASE(SnapshotIsNewestFirst) {
    shield::lua::SlowCallRing ring;
    ring.enable(1);
    for (const char* svc : {"first", "second", "third"}) {
        const uint64_t begin = steady_ms_now();
        age(3);  // each span ages past the threshold in flight
        ring.maybe_record(begin, svc, "callee", true);
    }
    auto snap = ring.snapshot();
    BOOST_REQUIRE_EQUAL(snap.recent.size(), 3U);
    BOOST_CHECK_EQUAL(snap.recent[0].caller, "third");
    BOOST_CHECK_EQUAL(snap.recent[1].caller, "second");
    BOOST_CHECK_EQUAL(snap.recent[2].caller, "first");
}

// Wrapping evicts the oldest sample; total_recorded keeps counting past
// capacity and the newest sample leads the snapshot.
BOOST_AUTO_TEST_CASE(RingWrapsOldestEvicted) {
    shield::lua::SlowCallRing ring;
    ring.enable(1);
    constexpr int kRecords =
        static_cast<int>(shield::lua::SlowCallRing::kCapacity + 1);
    for (int i = 0; i < kRecords; ++i) {
        const uint64_t begin = steady_ms_now();
        age(2);  // age past the threshold on every span
        ring.maybe_record(begin, "svc_" + std::to_string(i), "callee", true);
    }
    auto snap = ring.snapshot();
    BOOST_CHECK_EQUAL(snap.recent.size(), shield::lua::SlowCallRing::kCapacity);
    BOOST_CHECK_EQUAL(snap.total_recorded, static_cast<uint64_t>(kRecords));
    // Newest leads; the evicted one (svc_0) is gone; the oldest survivor
    // (svc_1) trails.
    BOOST_CHECK_EQUAL(snap.recent[0].caller,
                      "svc_" + std::to_string(kRecords - 1));
    BOOST_CHECK_EQUAL(snap.recent.back().caller, "svc_1");
}

// The singleton is one process-wide object, and arming it via instance()
// arms the same gate every call path reads.
BOOST_AUTO_TEST_CASE(InstanceIsProcessWide) {
    auto& a = shield::lua::SlowCallRing::instance();
    auto& b = shield::lua::SlowCallRing::instance();
    BOOST_CHECK(&a == &b);
    a.enable(60'000);
    BOOST_CHECK(shield::lua::SlowCallRing::instance().enabled());
}

BOOST_AUTO_TEST_SUITE_END()
