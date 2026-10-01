// GlobalManager unit tests (shield_global P0). Constructed directly from
// GlobalConfig: no global config store, no Lua, no bootstrap. Config
// parsing tests opt in via global_config().set().
#define BOOST_TEST_MODULE GlobalManagerTests
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/global/global_manager.hpp"

using shield::global::cron_next;
using shield::global::CronFields;
using shield::global::DeadEntry;
using shield::global::GlobalConfig;
using shield::global::GlobalManager;
using shield::global::LockInfo;
using shield::global::LockStatus;
using shield::global::NackResult;
using shield::global::parse_cron;
using shield::global::RankEntry;
using shield::global::RateLimitConfig;
using shield::global::ReliableDelivery;
using shield::global::SchedInfo;
using shield::global::validate_global_config;

namespace {

constexpr std::uint64_t kMinute = 60000;
constexpr std::uint64_t kHour = 3600000;
constexpr std::uint64_t kDay = 86400000;

GlobalConfig default_config() { return GlobalConfig{}; }

/// Fills the given bitset for every value in [lo, hi].
std::uint64_t bits_for(int lo, int hi) {
    std::uint64_t bits = 0;
    for (int v = lo; v <= hi; ++v) {
        bits |= (std::uint64_t(1) << v);
    }
    return bits;
}

bool wait_until(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

}  // namespace

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(GlobalConfigSuite)

BOOST_AUTO_TEST_CASE(DefaultsValidate) {
    GlobalConfig config;
    BOOST_CHECK(validate_global_config(config, nullptr));
}

BOOST_AUTO_TEST_CASE(RejectsZeroCacheSize) {
    GlobalConfig config;
    config.cache_max_size = 0;
    std::string error;
    BOOST_CHECK(!validate_global_config(config, &error));
    BOOST_CHECK_NE(error.find("global.cache.max_size"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(RejectsTinySchedulerTick) {
    GlobalConfig config;
    config.scheduler_tick_ms = 9;
    std::string error;
    BOOST_CHECK(!validate_global_config(config, &error));
    BOOST_CHECK_NE(error.find("global.scheduler.tick_ms"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(ParsesOverridesFromGlobalConfig) {
    auto& cfg = shield::config::global_config();
    cfg.set("global.cache.max_size", 5);
    cfg.set("global.cache.default_ttl", 1234);
    cfg.set("global.scheduler.tick_ms", 50);
    GlobalConfig parsed;
    std::string error;
    BOOST_CHECK(GlobalConfig::from_global_config(&parsed, &error));
    BOOST_CHECK_EQUAL(parsed.cache_max_size, 5);
    BOOST_CHECK_EQUAL(parsed.cache_default_ttl_ms, 1234);
    BOOST_CHECK_EQUAL(parsed.scheduler_tick_ms, 50);
    cfg.set("global.cache.max_size", 10000);
    cfg.set("global.cache.default_ttl", 60000);
    cfg.set("global.scheduler.tick_ms", 250);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Cron
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(CronParsing)

BOOST_AUTO_TEST_CASE(ParsesWildcardAndSimpleValues) {
    CronFields f;
    std::string error;
    BOOST_CHECK(parse_cron("* * * * *", &f, &error));
    // `*` fills the full domain mask in every field.
    BOOST_CHECK_EQUAL(f.minute, bits_for(0, 59));
    BOOST_CHECK_EQUAL(f.hour, bits_for(0, 23));
    BOOST_CHECK_EQUAL(f.dom, bits_for(1, 31));
    BOOST_CHECK_EQUAL(f.month, bits_for(1, 12));
    BOOST_CHECK_EQUAL(f.dow, bits_for(0, 6));
}

BOOST_AUTO_TEST_CASE(ParsesRangesStepsAndLists) {
    CronFields f;
    BOOST_CHECK(parse_cron("*/5 1-3 10,20 2-4/2 1,3-5", &f, nullptr));
    std::uint64_t every5 = 0;
    for (int v = 0; v <= 55; v += 5) {
        every5 |= (std::uint64_t(1) << v);
    }
    BOOST_CHECK_EQUAL(f.minute, every5);
    // 1,2,3
    BOOST_CHECK_EQUAL(f.hour, 2 | 4 | 8);
    // 10,20
    BOOST_CHECK_EQUAL(f.dom, (1u << 10) | (1u << 20));
    // 2-4/2 => 2,4
    BOOST_CHECK_EQUAL(f.month, (1u << 2) | (1u << 4));
    // 1,3,4,5
    BOOST_CHECK_EQUAL(f.dow, 2 | 8 | 16 | 32);
}

BOOST_AUTO_TEST_CASE(RejectsMalformedExpressions) {
    CronFields f;
    std::string error;
    BOOST_CHECK(!parse_cron("", &f, &error));
    BOOST_CHECK(!parse_cron("* * * *", &f, &error));
    BOOST_CHECK(!parse_cron("* * * * * *", &f, &error));
    BOOST_CHECK(!parse_cron("60 * * * *", &f, &error));
    BOOST_CHECK(!parse_cron("* 24 * * *", &f, &error));
    BOOST_CHECK(!parse_cron("* * 0 * *", &f, &error));
    BOOST_CHECK(!parse_cron("* * 32 * *", &f, &error));
    BOOST_CHECK(!parse_cron("* * * 13 *", &f, &error));
    BOOST_CHECK(!parse_cron("* * * * 7", &f, &error));
    BOOST_CHECK(!parse_cron("5-1 * * * *", &f, &error));
    BOOST_CHECK(!parse_cron("*/0 * * * *", &f, &error));
    BOOST_CHECK(!parse_cron("a * * * *", &f, &error));
    BOOST_CHECK(!parse_cron("1,,2 * * * *", &f, &error));
    BOOST_CHECK(!parse_cron("1- * * * *", &f, &error));
    BOOST_CHECK(!parse_cron("*/ * * * *", &f, &error));
    BOOST_CHECK_NE(error.size(), 0u);
}

BOOST_AUTO_TEST_CASE(NextMatchesKnownMoments) {
    CronFields f;
    // 2026-09-14 00:00:00 UTC is a Monday (dow = 1).
    const std::uint64_t monday = 1789344000000ULL;
    BOOST_CHECK(parse_cron("30 4 * * *", &f, nullptr));
    std::uint64_t next = cron_next(f, monday);
    // 04:30 the same Monday.
    BOOST_CHECK_EQUAL(next, monday + 4 * kHour + 30 * kMinute);
    // From a moment past 04:30 the next hit is the following day.
    BOOST_CHECK_EQUAL(cron_next(f, monday + 5 * kHour),
                      monday + kDay + 4 * kHour + 30 * kMinute);

    // Every 5 minutes.
    BOOST_CHECK(parse_cron("*/5 * * * *", &f, nullptr));
    next = cron_next(f, monday + 3 * kMinute);
    BOOST_CHECK_EQUAL(next, monday + 5 * kMinute);

    // Weekdays only at 12:00 (Sep 14 2026 = Monday): same day.
    BOOST_CHECK(parse_cron("0 12 * * 1-5", &f, nullptr));
    BOOST_CHECK_EQUAL(cron_next(f, monday), monday + 12 * kHour);
    // Saturday Sep 19 00:00 -> next hit is Monday Sep 21 12:00.
    const std::uint64_t saturday = monday + 5 * kDay;
    BOOST_CHECK_EQUAL(cron_next(f, saturday), monday + 7 * kDay + 12 * kHour);

    // Monthly on the 1st: Sep 14 -> Oct 1 00:00 (30 days in Sep 2026).
    BOOST_CHECK(parse_cron("0 0 1 * *", &f, nullptr));
    BOOST_CHECK_EQUAL(cron_next(f, monday + kHour), monday + 17 * kDay);

    // dom/dow OR semantics: both restricted, either may match. From
    // Monday Sep 14 01:00 the next hit is the next Monday (dow side).
    BOOST_CHECK(parse_cron("0 0 1 * 1", &f, nullptr));
    BOOST_CHECK_EQUAL(cron_next(f, monday + kHour), monday + 7 * kDay);

    // Impossible combination (Feb 31) yields no match.
    BOOST_CHECK(parse_cron("0 0 31 2 *", &f, nullptr));
    BOOST_CHECK_EQUAL(cron_next(f, monday), 0u);
}

BOOST_AUTO_TEST_CASE(NextIsStrictlyAfter) {
    CronFields f;
    BOOST_CHECK(parse_cron("* * * * *", &f, nullptr));
    const std::uint64_t at = 1760428800123ULL;
    // The next whole minute, even when `from` is already aligned.
    BOOST_CHECK_EQUAL(cron_next(f, at), ((at / kMinute) + 1) * kMinute);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Global data
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(GlobalDataSuite)

BOOST_AUTO_TEST_CASE(SetGetDeleteRoundTrip) {
    GlobalManager gm(default_config());
    std::string value;
    BOOST_CHECK(!gm.data_get("missing", &value));
    gm.data_set("k", "42", 0);
    BOOST_CHECK(gm.data_get("k", &value));
    BOOST_CHECK_EQUAL(value, "42");
    BOOST_CHECK(gm.data_delete("k"));
    BOOST_CHECK(!gm.data_delete("k"));
    BOOST_CHECK_EQUAL(gm.data_size(), 0u);
}

BOOST_AUTO_TEST_CASE(TtlExpiresLazily) {
    GlobalManager gm(default_config());
    gm.data_set("k", "v", 30);
    std::string value;
    BOOST_CHECK(gm.data_get("k", &value));
    BOOST_CHECK(wait_until([&] { return !gm.data_get("k", &value); }, 2000));
    BOOST_CHECK_EQUAL(gm.data_size(), 0u);
}

BOOST_AUTO_TEST_CASE(IncrDecrAndErrors) {
    GlobalManager gm(default_config());
    std::int64_t out = 0;
    BOOST_CHECK(gm.data_incr_by("c", 1, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 1);
    BOOST_CHECK(gm.data_incr_by("c", 41, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 42);
    BOOST_CHECK(gm.data_incr_by("c", -2, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 40);
    gm.data_set("text", "\"hello\"", 0);
    std::string error;
    BOOST_CHECK(!gm.data_incr_by("text", 1, &out, &error));
    BOOST_CHECK_NE(error.find("not an integer"), std::string::npos);
    // A partially numeric value is still rejected.
    gm.data_set("partial", "12x", 0);
    BOOST_CHECK(!gm.data_incr_by("partial", 1, &out, nullptr));
    // An expired value resets to delta (no stale carry-over).
    gm.data_set("gone", "100", 20);
    BOOST_CHECK(
        wait_until([&] { return !gm.data_get("gone", nullptr); }, 2000));
    BOOST_CHECK(gm.data_incr_by("gone", 5, &out, nullptr));
    BOOST_CHECK_EQUAL(out, 5);
}

BOOST_AUTO_TEST_CASE(MgetMset) {
    GlobalManager gm(default_config());
    std::vector<std::pair<std::string, std::string>> kvs = {{"a", "1"},
                                                            {"b", "2"}};
    BOOST_CHECK(gm.data_mset(kvs, 0, nullptr));
    auto values = gm.data_mget({"a", "b", "c"});
    BOOST_CHECK(values[0].has_value() && *values[0] == "1");
    BOOST_CHECK(values[1].has_value() && *values[1] == "2");
    BOOST_CHECK(!values[2].has_value());
    std::string error;
    kvs.push_back({"", "v"});
    BOOST_CHECK(!gm.data_mset(kvs, 0, &error));
    BOOST_CHECK_NE(error.find("empty"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(WriteInvalidatesCache) {
    GlobalManager gm(default_config());
    gm.data_set("k", "\"v1\"", 0);
    std::string value;
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    gm.data_set("k", "\"v2\"", 0);
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(value, "\"v2\"");
    BOOST_CHECK(gm.data_delete("k"));
    BOOST_CHECK(!gm.cache_get("k", 60000, &value));
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Local cache
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(LocalCacheSuite)

BOOST_AUTO_TEST_CASE(MissFillsFromDataAndCounts) {
    GlobalManager gm(default_config());
    gm.data_set("k", "\"v\"", 0);
    std::string value;
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(value, "\"v\"");
    // Second read is a hit.
    BOOST_CHECK(gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(gm.cache_hits(), 1u);
    BOOST_CHECK_EQUAL(gm.cache_misses(), 1u);
    // Unknown key stays a miss.
    BOOST_CHECK(!gm.cache_get("nope", 60000, &value));
    BOOST_CHECK_EQUAL(gm.cache_misses(), 2u);
    BOOST_CHECK_EQUAL(gm.cache_size(), 1u);
}

BOOST_AUTO_TEST_CASE(TtlExpiryAndInvalidateAndClear) {
    GlobalManager gm(default_config());
    gm.data_set("k", "1", 0);
    std::string value;
    const std::uint64_t misses_before = gm.cache_misses();
    BOOST_CHECK(gm.cache_get("k", 30, &value));
    // After the cache TTL the entry expires; the read refills from the
    // data domain (another miss, never a stale cached copy).
    BOOST_CHECK(
        wait_until([&] { return gm.cache_misses() > misses_before; }, 2000));
    // invalidate drops a live entry.
    gm.cache_get("k", 60000, &value);
    gm.cache_invalidate("k");
    BOOST_CHECK_EQUAL(gm.cache_size(), 0u);
    // clear wipes everything.
    gm.cache_get("k", 60000, &value);
    gm.cache_clear();
    BOOST_CHECK_EQUAL(gm.cache_size(), 0u);
}

BOOST_AUTO_TEST_CASE(LruEviction) {
    GlobalConfig config;
    config.cache_max_size = 2;
    GlobalManager gm(config);
    gm.data_set("a", "1", 0);
    gm.data_set("b", "2", 0);
    gm.data_set("c", "3", 0);
    std::string value;
    BOOST_CHECK(gm.cache_get("a", 60000, &value));
    BOOST_CHECK(gm.cache_get("b", 60000, &value));
    BOOST_CHECK_EQUAL(gm.cache_size(), 2u);
    // Touching "a" makes "b" the LRU victim.
    BOOST_CHECK(gm.cache_get("a", 60000, &value));
    BOOST_CHECK(gm.cache_get("c", 60000, &value));
    BOOST_CHECK_EQUAL(gm.cache_size(), 2u);
    // "b" was evicted; "a" survives as a hit.
    BOOST_CHECK(gm.cache_get("a", 60000, &value));
    BOOST_CHECK_EQUAL(gm.cache_hits() + gm.cache_misses(), 5u);
}

BOOST_AUTO_TEST_CASE(ExpiredDataEntryIsNotFilledIntoCache) {
    GlobalManager gm(default_config());
    gm.data_set("k", "1", 30);
    // Sleep past the data TTL (no probing reads: they would cache the
    // still-live value), then the cache fill must observe the expiry.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    std::string value;
    BOOST_CHECK(!gm.cache_get("k", 60000, &value));
    BOOST_CHECK_EQUAL(gm.cache_size(), 0u);
    BOOST_CHECK_EQUAL(gm.data_size(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Exclusive locks
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(MutexSuite)

BOOST_AUTO_TEST_CASE(AcquireReleaseExcludesOthers) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "A", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "B", 0) == LockStatus::kBusy);
    // Reentrant for the same owner.
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "A", 0) == LockStatus::kOk);
    // Release by a non-holder fails.
    BOOST_CHECK(gm.mutex_release("mutex", "m", "B") == LockStatus::kNotOwner);
    BOOST_CHECK(gm.mutex_release("mutex", "m", "A") == LockStatus::kOk);
    // Still held once (reentrant count).
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "B", 0) == LockStatus::kBusy);
    BOOST_CHECK(gm.mutex_release("mutex", "m", "A") == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "B", 0) == LockStatus::kOk);
    // Unknown name release is a not-owner result.
    BOOST_CHECK(gm.mutex_release("mutex", "nope", "B") ==
                LockStatus::kNotOwner);
}

BOOST_AUTO_TEST_CASE(ReentrantTtlRefresh) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "A", 5000) == LockStatus::kOk);
    LockInfo info = gm.mutex_info("mutex", "m");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.count, 1u);
    BOOST_CHECK_GT(info.ttl_remaining_ms, 0u);
    BOOST_CHECK_LE(info.ttl_remaining_ms, 5000u);
    // A reentrant acquire refreshes the TTL.
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "A", 60000) == LockStatus::kOk);
    info = gm.mutex_info("mutex", "m");
    BOOST_CHECK_GT(info.ttl_remaining_ms, 5000u);
    BOOST_CHECK(!gm.mutex_info("mutex", "missing").exists);
}

BOOST_AUTO_TEST_CASE(TtlExpiryEnablesTakeover) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "A", 40) == LockStatus::kOk);
    BOOST_CHECK(wait_until(
        [&] {
            return gm.mutex_acquire("mutex", "m", "B", 0) == LockStatus::kOk;
        },
        2000));
    // The stale holder can no longer release or extend.
    BOOST_CHECK(gm.mutex_release("mutex", "m", "A") == LockStatus::kNotOwner);
    BOOST_CHECK(!gm.mutex_extend("mutex", "m", "A", 1000));
}

BOOST_AUTO_TEST_CASE(ExtendOnlyForLiveOwner) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.mutex_acquire("mutex", "m", "A", 100000) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_extend("mutex", "m", "A", 200000));
    LockInfo info = gm.mutex_info("mutex", "m");
    BOOST_CHECK_GT(info.ttl_remaining_ms, 100000u);
    BOOST_CHECK(!gm.mutex_extend("mutex", "m", "B", 200000));
    // extend(0) clears the TTL.
    BOOST_CHECK(gm.mutex_extend("mutex", "m", "A", 0));
    info = gm.mutex_info("mutex", "m");
    BOOST_CHECK_EQUAL(info.ttl_remaining_ms, 0u);
}

BOOST_AUTO_TEST_CASE(SpinlockRegistryIsIndependent) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.mutex_acquire("spinlock", "s", "A", 0) == LockStatus::kOk);
    // The same name under the mutex registry is a different lock.
    BOOST_CHECK(gm.mutex_acquire("mutex", "s", "B", 0) == LockStatus::kOk);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("spinlock"), 1u);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("mutex"), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Reader/writer locks
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(RwLockSuite)

BOOST_AUTO_TEST_CASE(ReadersShareWriterExcludes) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.rw_read_acquire("rw", "r1") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_read_acquire("rw", "r2") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_write_acquire("rw", "w", 0) == LockStatus::kBusy);
    // Reentrant read for the same token.
    BOOST_CHECK(gm.rw_read_acquire("rw", "r1") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_read_release("rw", "r1") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_read_release("rw", "r1") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_read_release("rw", "r2") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_write_acquire("rw", "w", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.rw_read_acquire("rw", "r1") == LockStatus::kBusy);
    BOOST_CHECK(gm.rw_write_acquire("rw", "w", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.rw_write_release("rw", "w") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_write_release("rw", "w") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_read_acquire("rw", "r1") == LockStatus::kOk);
}

BOOST_AUTO_TEST_CASE(WriteTtlExpiryAndExtend) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.rw_write_acquire("rw", "w", 40) == LockStatus::kOk);
    BOOST_CHECK(wait_until(
        [&] { return gm.rw_write_acquire("rw", "w2", 0) == LockStatus::kOk; },
        2000));
    // Stale writer cannot release/extend.
    BOOST_CHECK(gm.rw_write_release("rw", "w") == LockStatus::kNotOwner);
    BOOST_CHECK(!gm.rw_write_extend("rw", "w", 1000));
    // A stale write TTL also lets a reader in.
    BOOST_CHECK(gm.rw_write_extend("rw", "w2", 30));
    BOOST_CHECK(wait_until(
        [&] { return gm.rw_read_acquire("rw", "r") == LockStatus::kOk; },
        2000));
}

BOOST_AUTO_TEST_CASE(InfoAndCounts) {
    GlobalManager gm(default_config());
    BOOST_CHECK(!gm.rwlock_info("rw").exists);
    BOOST_CHECK(gm.rw_write_acquire("rw", "w", 5000) == LockStatus::kOk);
    auto info = gm.rwlock_info("rw");
    BOOST_CHECK(info.exists);
    BOOST_CHECK_EQUAL(info.write_owner, "w");
    BOOST_CHECK_EQUAL(info.write_count, 1u);
    BOOST_CHECK_GT(info.write_ttl_remaining_ms, 0u);
    BOOST_CHECK_EQUAL(info.readers, 0u);
    // Not-owner releases.
    BOOST_CHECK(gm.rw_read_release("rw", "ghost") == LockStatus::kNotOwner);
    BOOST_CHECK(gm.rw_write_release("rw", "ghost") == LockStatus::kNotOwner);
    BOOST_CHECK_EQUAL(gm.rwlock_count(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Leaderboards
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(RankSuite)

BOOST_AUTO_TEST_CASE(UpdateTopRangePosition) {
    GlobalManager gm(default_config());
    gm.rank_update("b", "p3", 1100);
    gm.rank_update("b", "p1", 1000);
    gm.rank_update("b", "p2", 1000);  // tie: uid asc puts p1 before p2
    auto top = gm.rank_top("b", 2);
    BOOST_REQUIRE_EQUAL(top.size(), 2u);
    BOOST_CHECK_EQUAL(top[0].uid, "p3");
    BOOST_CHECK_EQUAL(top[0].rank, 1u);
    BOOST_CHECK_EQUAL(top[1].uid, "p1");
    BOOST_CHECK_EQUAL(top[1].rank, 2u);
    auto range = gm.rank_range("b", 2, 3);
    BOOST_REQUIRE_EQUAL(range.size(), 2u);
    BOOST_CHECK_EQUAL(range[0].uid, "p1");
    BOOST_CHECK_EQUAL(range[1].uid, "p2");
    // Out-of-range bounds clamp.
    BOOST_CHECK_EQUAL(gm.rank_range("b", 5, 9).size(), 0u);
    BOOST_CHECK_EQUAL(gm.rank_range("b", 0, 9).size(), 0u);
    BOOST_CHECK_EQUAL(gm.rank_range("b", 3, 1).size(), 0u);
    BOOST_CHECK_EQUAL(gm.rank_position("b", "p2").value(), 3u);
    BOOST_CHECK(!gm.rank_position("b", "ghost").has_value());
    BOOST_CHECK_EQUAL(gm.rank_score("b", "p1").value(), 1000.0);
    BOOST_CHECK(!gm.rank_score("missing", "p1").has_value());
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 3u);
    BOOST_CHECK_EQUAL(gm.rank_top("b", 0).size(), 0u);
    BOOST_CHECK_EQUAL(gm.rank_top("missing", 5).size(), 0u);
}

BOOST_AUTO_TEST_CASE(MupdateRemoveClearAndAggregates) {
    GlobalManager gm(default_config());
    gm.rank_mupdate("b", {{"a", 1.0}, {"z", 2.0}});
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 2u);
    BOOST_CHECK(gm.rank_remove("b", "a"));
    BOOST_CHECK(!gm.rank_remove("b", "a"));
    BOOST_CHECK(!gm.rank_remove("missing", "a"));
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 1u);
    gm.rank_update("b2", "x", 5.0);
    BOOST_CHECK_EQUAL(gm.rank_board_count(), 2u);
    BOOST_CHECK_EQUAL(gm.rank_total_members(), 2u);
    gm.rank_clear("b");
    BOOST_CHECK_EQUAL(gm.rank_count("b"), 0u);
    BOOST_CHECK_EQUAL(gm.rank_board_count(), 1u);
    BOOST_CHECK_EQUAL(gm.rank_total_members(), 1u);
}

BOOST_AUTO_TEST_CASE(RangeByScoreAndAround) {
    GlobalManager gm(default_config());
    for (int i = 1; i <= 5; ++i) {
        gm.rank_update("b", "u" + std::to_string(i), i * 10.0);
    }
    auto window = gm.rank_range_by_score("b", 20.0, 40.0);
    BOOST_REQUIRE_EQUAL(window.size(), 3u);
    // Rank order: u4 (40) first, u2 (20) last.
    BOOST_CHECK_EQUAL(window[0].uid, "u4");
    BOOST_CHECK_EQUAL(window[2].uid, "u2");
    BOOST_CHECK_EQUAL(gm.rank_range_by_score("b", 40.0, 20.0).size(), 0u);
    BOOST_CHECK_EQUAL(gm.rank_range_by_score("missing", 0, 9).size(), 0u);

    auto around = gm.rank_around("b", "u3", 1);
    BOOST_REQUIRE_EQUAL(around.above.size(), 1u);
    BOOST_CHECK_EQUAL(around.above[0].uid, "u4");
    BOOST_REQUIRE_EQUAL(around.below.size(), 1u);
    BOOST_CHECK_EQUAL(around.below[0].uid, "u2");
    BOOST_CHECK(around.target.has_value());
    BOOST_CHECK_EQUAL(around.target->uid, "u3");
    BOOST_CHECK_EQUAL(around.target->rank, 3u);
    // Around the leader: no above entries.
    around = gm.rank_around("b", "u5", 2);
    BOOST_CHECK_EQUAL(around.above.size(), 0u);
    BOOST_REQUIRE_EQUAL(around.below.size(), 2u);
    // Unknown uid / board.
    BOOST_CHECK(!gm.rank_around("b", "ghost", 2).target.has_value());
    BOOST_CHECK(!gm.rank_around("missing", "u1", 2).target.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Queues
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(QueueSuite)

BOOST_AUTO_TEST_CASE(FifoPushBatchPopLengthPurge) {
    GlobalManager gm(default_config());
    std::string out;
    BOOST_CHECK(!gm.queue_pop("q", &out));
    gm.queue_push("q", "a");
    gm.queue_push_batch("q", {"b", "c"});
    BOOST_CHECK_EQUAL(gm.queue_length("q"), 3u);
    BOOST_CHECK(gm.queue_pop("q", &out) && out == "a");
    BOOST_CHECK(gm.queue_pop("q", &out) && out == "b");
    BOOST_CHECK(gm.queue_pop("q", &out) && out == "c");
    BOOST_CHECK(!gm.queue_pop("q", &out));
    gm.queue_push("q", "d");
    BOOST_CHECK_EQUAL(gm.queue_count(), 1u);
    gm.queue_purge("q");
    BOOST_CHECK_EQUAL(gm.queue_length("q"), 0u);
    BOOST_CHECK_EQUAL(gm.queue_count(), 0u);
}

BOOST_AUTO_TEST_CASE(DelayQueueScheduling) {
    GlobalManager gm(default_config());
    std::string out;
    gm.delay_push("d", "late", 80000);
    gm.delay_push("d", "now", 0);
    gm.delay_push_at("d", "at", GlobalManager::now_ms() - 5);
    BOOST_CHECK_EQUAL(gm.delay_pending("d"), 1u);
    BOOST_CHECK_EQUAL(gm.delay_ready("d"), 2u);
    BOOST_CHECK(gm.delay_pop("d", &out) && (out == "now" || out == "at"));
    BOOST_CHECK(gm.delay_pop("d", &out) && (out == "now" || out == "at"));
    BOOST_CHECK(!gm.delay_pop("d", &out));
    BOOST_CHECK_EQUAL(gm.delay_queue_count(), 1u);
    gm.delay_purge("d");
    BOOST_CHECK_EQUAL(gm.delay_queue_count(), 0u);
    BOOST_CHECK_EQUAL(gm.delay_pending("missing"), 0u);
    BOOST_CHECK_EQUAL(gm.delay_ready("missing"), 0u);
}

// True-arm edges behind the branch long-tail: delay_pop on a queue that was
// never created, and on a queued item whose due time is still ahead; the
// rank window guards (from==0, to<from); rank_clear on a board that never
// existed.
BOOST_AUTO_TEST_CASE(RankAndDelayTrueArmEdges) {
    GlobalManager gm(default_config());
    gm.rank_update("rb", "a", 1.0);
    BOOST_CHECK(gm.rank_range("no-such-board", 1, 5).empty());  // absent board
    BOOST_CHECK(gm.rank_range("rb", 0, 10).empty());            // from == 0
    BOOST_CHECK(gm.rank_range("rb", 5, 3).empty());             // to < from
    gm.rank_clear("never-created-board");                       // absent no-op
    std::string out;
    BOOST_CHECK(!gm.delay_pop("no-such-queue", &out));
    gm.delay_push("drain", "x", 0);
    BOOST_CHECK(gm.delay_pop("drain", &out));
    BOOST_CHECK(!gm.delay_pop("drain", &out));  // exists but empty
    gm.delay_push("future", "x", 600000);
    BOOST_CHECK(!gm.delay_pop("future", &out));  // not yet due
    BOOST_CHECK_EQUAL(gm.delay_pending("future"), 1u);
}

BOOST_AUTO_TEST_CASE(PriorityAndBroadcastQueueCounts) {
    GlobalManager gm(default_config());
    std::string out;
    gm.priority_push("p2", "low", 20);
    gm.priority_push("p1", "high", 5);
    BOOST_CHECK(gm.priority_pop("p1", &out) && out == "high");
    BOOST_CHECK_EQUAL(gm.priority_length("p2"), 1u);
    BOOST_CHECK_EQUAL(gm.priority_queue_count(), 2u);
    gm.priority_purge("p2");
    BOOST_CHECK_EQUAL(gm.priority_queue_count(), 1u);

    const auto first = gm.broadcast_push("b", "one");
    const auto second = gm.broadcast_push("b", "two");
    BOOST_CHECK_EQUAL(second, first + 1);
    gm.broadcast_push("b2", "other");
    BOOST_CHECK_EQUAL(gm.broadcast_history_size("b"), 2u);
    BOOST_CHECK_EQUAL(gm.broadcast_queue_count(), 2u);
    gm.broadcast_purge("b2");
    BOOST_CHECK_EQUAL(gm.broadcast_queue_count(), 1u);
}

BOOST_AUTO_TEST_CASE(ReliableAckAndNackRequeue) {
    GlobalManager gm(default_config());
    gm.reliable_configure("r", 3);
    ReliableDelivery delivery;
    BOOST_CHECK(!gm.reliable_pop("r", &delivery));
    gm.reliable_push("r", "job1");
    BOOST_REQUIRE(gm.reliable_pop("r", &delivery));
    BOOST_CHECK_EQUAL(delivery.payload, "job1");
    BOOST_CHECK_EQUAL(gm.reliable_inflight("r"), 1u);
    // ack of an unknown id fails.
    BOOST_CHECK(!gm.reliable_ack("r", 999));
    BOOST_CHECK(gm.reliable_ack("r", delivery.delivery_id));
    BOOST_CHECK_EQUAL(gm.reliable_inflight("r"), 0u);
    // Double ack fails.
    BOOST_CHECK(!gm.reliable_ack("r", delivery.delivery_id));

    // nack requeues after the retry delay, preserving retry state.
    gm.reliable_push("r", "job2");
    BOOST_REQUIRE(gm.reliable_pop("r", &delivery));
    BOOST_CHECK_EQUAL(delivery.payload, "job2");
    BOOST_CHECK(gm.reliable_nack("r", delivery.delivery_id, 30) ==
                NackResult::kRequeued);
    // Not yet due.
    ReliableDelivery again;
    BOOST_CHECK(!gm.reliable_pop("r", &again));
    BOOST_CHECK(
        wait_until([&] { return gm.reliable_pop("r", &delivery); }, 2000));
    BOOST_CHECK_EQUAL(delivery.payload, "job2");
    // nack of an unknown id reports not-found.
    BOOST_CHECK(gm.reliable_nack("r", 999, 0) == NackResult::kNotFound);
    BOOST_CHECK_EQUAL(gm.reliable_queue_count(), 1u);
}

BOOST_AUTO_TEST_CASE(ReliableNackExhaustionMovesToDeadLetter) {
    GlobalManager gm(default_config());
    gm.reliable_configure("r", 2);  // dead after the 2nd nack
    ReliableDelivery delivery;
    gm.reliable_push("r", "poison");
    BOOST_REQUIRE(gm.reliable_pop("r", &delivery));
    BOOST_CHECK(gm.reliable_nack("r", delivery.delivery_id, 0) ==
                NackResult::kRequeued);
    BOOST_REQUIRE(gm.reliable_pop("r", &delivery));
    BOOST_CHECK(gm.reliable_nack("r", delivery.delivery_id, 0) ==
                NackResult::kDead);
    BOOST_CHECK_EQUAL(gm.reliable_dead_size("r"), 1u);
    auto dead = gm.reliable_dead_range("r", 0, 10);
    BOOST_REQUIRE_EQUAL(dead.size(), 1u);
    BOOST_CHECK_EQUAL(dead[0].payload, "poison");
    BOOST_CHECK_EQUAL(dead[0].retries, 2u);
    // Range bounds clamp.
    BOOST_CHECK_EQUAL(gm.reliable_dead_range("r", 5, 9).size(), 0u);
    BOOST_CHECK_EQUAL(gm.reliable_dead_range("missing", 0, 9).size(), 0u);
    gm.reliable_dead_purge("r");
    BOOST_CHECK_EQUAL(gm.reliable_dead_size("r"), 0u);
    // The dead delivery no longer nacks.
    BOOST_CHECK(gm.reliable_nack("r", delivery.delivery_id, 0) ==
                NackResult::kNotFound);
}

BOOST_AUTO_TEST_CASE(ReliableConfigureKeepsDefaultOnInvalid) {
    GlobalManager gm(default_config());
    gm.reliable_configure("r", 0);  // keeps default 3
    gm.reliable_configure("r", -5);
    ReliableDelivery delivery;
    gm.reliable_push("r", "x");
    BOOST_REQUIRE(gm.reliable_pop("r", &delivery));
    BOOST_CHECK(gm.reliable_nack("r", delivery.delivery_id, 0) ==
                NackResult::kRequeued);
    BOOST_REQUIRE(gm.reliable_pop("r", &delivery));
    BOOST_CHECK(gm.reliable_nack("r", delivery.delivery_id, 0) ==
                NackResult::kRequeued);
    BOOST_REQUIRE(gm.reliable_pop("r", &delivery));
    // Third nack hits the default cap of 3.
    BOOST_CHECK(gm.reliable_nack("r", delivery.delivery_id, 0) ==
                NackResult::kDead);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Rate limiters
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(RateLimitSuite)

// key_count / purge are the introspection pair behind /ops global status;
// both backends keep separate per-key state, so exercise each shape.
BOOST_AUTO_TEST_CASE(KeyCountAndPurgeCoverBothBackends) {
    GlobalManager gm(default_config());
    RateLimitConfig bucket;
    bucket.rate = 10.0;
    bucket.burst = 5.0;
    gm.rate_limit_configure("rl_bucket", bucket);
    BOOST_CHECK(gm.rate_limit_allow("rl_bucket", "a", 1.0).allowed);
    BOOST_CHECK(gm.rate_limit_allow("rl_bucket", "b", 1.0).allowed);
    BOOST_CHECK_EQUAL(gm.rate_limit_key_count("rl_bucket"), 2u);

    RateLimitConfig sliding;
    sliding.sliding = true;
    sliding.window_ms = 60000;
    sliding.max_requests = 10;
    gm.rate_limit_configure("rl_window", sliding);
    for (const char* key : {"x", "y", "z"}) {
        BOOST_CHECK(gm.rate_limit_allow("rl_window", key, 1.0).allowed);
    }
    BOOST_CHECK_EQUAL(gm.rate_limit_key_count("rl_window"), 3u);
    BOOST_CHECK_EQUAL(gm.rate_limit_key_count("rl_missing"), 0u);

    gm.rate_limit_purge("rl_bucket");
    gm.rate_limit_purge("rl_missing");  // no-op arm: unknown limiter name
    BOOST_CHECK_EQUAL(gm.rate_limit_key_count("rl_bucket"), 0u);
    // Post-purge consumption starts from a fresh full bucket.
    BOOST_CHECK(gm.rate_limit_allow("rl_bucket", "a", 5.0).allowed);
    BOOST_CHECK_EQUAL(gm.rate_limit_key_count("rl_bucket"), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(SchedulerSuite)

BOOST_AUTO_TEST_CASE(IntervalFiresAndRecomputes) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    std::vector<std::string> fired;
    gm.set_task_fire_fn(
        [&](const std::string& service, const std::string& task) {
            fired.push_back(service + ":" + task);
            return true;
        });
    std::string error;
    BOOST_CHECK(gm.sched_register("interval", "beat", "30", "svc", &error));
    gm.start();
    BOOST_CHECK(wait_until([&] { return fired.size() >= 2; }, 2000));
    gm.stop();
    auto info = gm.sched_get("beat");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_GE(info->run_count, 2u);
    BOOST_CHECK_GT(info->next_run_ms, 0u);
    BOOST_CHECK_GT(info->last_run_ms, 0u);
    BOOST_CHECK_EQUAL(info->type, "interval");
    BOOST_CHECK(!info->paused);
    BOOST_CHECK(!info->done);
}

BOOST_AUTO_TEST_CASE(PauseResumeOnceAndTrigger) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    int fired = 0;
    gm.set_task_fire_fn([&](const std::string&, const std::string& task) {
        if (task == "t") ++fired;
        return true;
    });
    std::string error;
    BOOST_CHECK(gm.sched_register("once", "t", "60000", "svc", &error));
    // trigger fires immediately without touching the schedule.
    BOOST_CHECK(gm.sched_trigger("t"));
    BOOST_CHECK_EQUAL(fired, 1);
    auto info = gm.sched_get("t");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(info->run_count, 1u);
    BOOST_CHECK_GT(info->next_run_ms, 0u);
    BOOST_CHECK(!info->done);
    // pause/resume on a once task re-arms from now.
    BOOST_CHECK(gm.sched_pause("t"));
    BOOST_CHECK(gm.sched_resume("t"));
    // Unknown task operations fail.
    BOOST_CHECK(!gm.sched_pause("ghost"));
    BOOST_CHECK(!gm.sched_resume("ghost"));
    BOOST_CHECK(!gm.sched_trigger("ghost"));
    BOOST_CHECK(!gm.sched_remove("ghost"));
    BOOST_CHECK(!gm.sched_get("ghost").has_value());
    // Registering a duplicate name fails.
    BOOST_CHECK(!gm.sched_register("interval", "t", "10", "svc", &error));
    BOOST_CHECK_NE(error.find("already exists"), std::string::npos);
    BOOST_CHECK(gm.sched_remove("t"));
    BOOST_CHECK(gm.sched_register("interval", "t", "10", "svc", &error));
}

BOOST_AUTO_TEST_CASE(CronValidationAndFiring) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    std::string error;
    BOOST_CHECK(!gm.sched_register("cron", "bad", "* * * *", "svc", &error));
    BOOST_CHECK(
        !gm.sched_register("cron", "bad2", "61 * * * *", "svc", &error));
    // A cron with no upcoming match is rejected.
    BOOST_CHECK(
        !gm.sched_register("cron", "bad3", "0 0 31 2 *", "svc", &error));
    BOOST_CHECK_NE(error.find("no upcoming match"), std::string::npos);
    // Every minute fires quickly.
    int fired = 0;
    gm.set_task_fire_fn([&](const std::string&, const std::string&) {
        ++fired;
        return true;
    });
    BOOST_CHECK(gm.sched_register("cron", "every", "* * * * *", "svc", &error));
    gm.start();
    BOOST_CHECK(wait_until([&] { return fired >= 1; }, 90000));
    gm.stop();
    BOOST_CHECK_GE(fired, 1);
}

BOOST_AUTO_TEST_CASE(InvalidTypeAndSchedule) {
    GlobalManager gm(default_config());
    std::string error;
    BOOST_CHECK(!gm.sched_register("weekly", "t", "1", "svc", &error));
    BOOST_CHECK_NE(error.find("unknown scheduler task type"),
                   std::string::npos);
    BOOST_CHECK(!gm.sched_register("interval", "t", "abc", "svc", &error));
    BOOST_CHECK(!gm.sched_register("once", "t", "-5", "svc", &error));
    BOOST_CHECK(!gm.sched_register("once", "t", "1.5", "svc", &error));
}

BOOST_AUTO_TEST_CASE(FireFailureDropsTaskAndListCounts) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    gm.set_task_fire_fn([](const std::string&, const std::string&) {
        return false;  // service gone
    });
    std::string error;
    BOOST_CHECK(gm.sched_register("interval", "gone", "10", "svc", &error));
    BOOST_CHECK(
        gm.sched_register("interval", "kept", "1000000", "svc2", &error));
    gm.start();
    BOOST_CHECK(
        wait_until([&] { return !gm.sched_get("gone").has_value(); }, 2000));
    gm.stop();
    // The healthy task survives.
    BOOST_CHECK(gm.sched_get("kept").has_value());
    BOOST_CHECK_EQUAL(gm.sched_list().size(), 1u);
    BOOST_CHECK_EQUAL(gm.sched_active_count(), 1u);
    // Pause removes it from the active count.
    gm.sched_pause("kept");
    BOOST_CHECK_EQUAL(gm.sched_active_count(), 0u);
}

BOOST_AUTO_TEST_CASE(PausedTasksDoNotBacklog) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(default_config());
    std::string error;
    BOOST_CHECK(gm.sched_register("interval", "p", "20", "svc", &error));
    gm.sched_pause("p");
    gm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gm.stop();
    auto info = gm.sched_get("p");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(info->run_count, 0u);
}

BOOST_AUTO_TEST_CASE(StopIsIdempotentAndStartIsIdempotent) {
    GlobalManager gm(default_config());
    gm.start();
    gm.start();
    // Double start leaves the data plane serving (no double tick thread).
    gm.data_set("cov_idem", "v1", 0);
    std::string out;
    BOOST_CHECK(gm.data_get("cov_idem", &out));
    BOOST_CHECK_EQUAL(out, "v1");
    gm.stop();
    gm.stop();
    // Double stop tears the tick thread down once; the in-memory data plane
    // stays readable and the value survives.
    BOOST_CHECK(gm.data_get("cov_idem", &out));
    BOOST_CHECK_EQUAL(out, "v1");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Error arms: the failure tails the happy-path matrices above never enter.
// Every arm here is driven by a real two-step scenario (an expired entry,
// an unknown id, a drained window) — no mocks.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(ErrorArmSuite)

// cron "*/a": the step text must be numeric (minute-field parse arm).
BOOST_AUTO_TEST_CASE(CronStepMustBeNumeric) {
    CronFields fields;
    std::string error;
    BOOST_CHECK(!parse_cron("*/a * * * *", &fields, &error));
    BOOST_CHECK_NE(error.find("step is not a number"), std::string::npos);
}

// Expired entries are erased in place on read across every read path:
// data_get, data_incr_by (fresh 0 afterwards), data_mget (mixed unknown
// and live), and the local cache (expired cache entry erased then
// refilled from the live value; a ttl=0 refill records no expiry).
BOOST_AUTO_TEST_CASE(ExpiredEntriesErasedOnRead) {
    GlobalManager gm(default_config());
    gm.data_set("k1", "1", 30);
    gm.data_set("k2", "2", 30);
    gm.data_set("k3", "3", 0);
    gm.data_set("ck", "v", 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    std::string out;
    BOOST_CHECK(!gm.data_get("k1", &out));  // erase-on-read arm

    std::int64_t n = 0;
    std::string error;
    BOOST_CHECK(gm.data_incr_by("k2", 5, &n, &error));  // fresh after expiry
    BOOST_CHECK_EQUAL(n, 5);

    auto mget = gm.data_mget({"nope", "k3"});
    BOOST_REQUIRE_EQUAL(mget.size(), 2u);
    BOOST_CHECK(!mget[0].has_value());
    BOOST_REQUIRE(mget[1].has_value());
    BOOST_CHECK_EQUAL(*mget[1], "3");

    // Expired cache entry: erased on read, refilled from live data.
    BOOST_CHECK(gm.cache_get("ck", 30, &out));  // fill (30ms expiry)
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BOOST_CHECK(gm.cache_get("ck", 30, &out));  // expire+refill arm
    BOOST_CHECK_EQUAL(out, "v");
    BOOST_CHECK(gm.cache_get("ck", 0, &out));  // hit with default-ttl entry

    // A zero configured default ttl records no expiry on refill.
    GlobalConfig no_ttl_cfg;
    no_ttl_cfg.cache_default_ttl_ms = 0;
    GlobalManager no_ttl(no_ttl_cfg);
    no_ttl.data_set("nk", "x", 0);
    BOOST_CHECK(no_ttl.cache_get("nk", 0, &out));
    BOOST_CHECK_EQUAL(out, "x");
    BOOST_CHECK(no_ttl.cache_get("nk", 0, &out));  // hit, never expires

    // A data expiry must also drop the cached copy: cache the key first,
    // let the data expire, then the read erases data and cache together.
    gm.data_set("cx", "v", 30);
    BOOST_CHECK(gm.cache_get("cx", 60000, &out));  // cached copy exists
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BOOST_CHECK(!gm.data_get("cx", &out));  // data gone, cache dropped
    BOOST_CHECK(!gm.cache_get("cx", 0, &out));
}

// Write-lock TTL bookkeeping: acquiring with a ttl records the write
// expiry; a live owner can extend, an expired or unknown lock cannot;
// read-release on an unknown lock is kNotOwner.
BOOST_AUTO_TEST_CASE(RwWriteTtlAndUnknownArms) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.rw_write_acquire("w", "o", 60000) == LockStatus::kOk);
    // Reentrant acquire by the live owner refreshes the recorded expiry.
    BOOST_CHECK(gm.rw_write_acquire("w", "o", 60000) == LockStatus::kOk);
    BOOST_CHECK(gm.rw_write_extend("w", "o", 60000));    // live owner: true
    BOOST_CHECK(!gm.rw_write_extend("never", "o", 10));  // unknown: false
    BOOST_CHECK(gm.rw_write_release("w", "o") == LockStatus::kOk);

    // Expired write lock: extend refuses.
    BOOST_CHECK(gm.rw_write_acquire("w2", "o", 40) == LockStatus::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BOOST_CHECK(!gm.rw_write_extend("w2", "o", 40000));

    // A stale writer is reset in place: a fresh acquire takes over with a
    // new owner, a new expiry, and no readers.
    BOOST_CHECK(gm.rw_write_acquire("w3", "o1", 40) == LockStatus::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BOOST_CHECK(gm.rw_write_acquire("w3", "o2", 60000) == LockStatus::kOk);
    BOOST_CHECK(gm.rw_write_release("w3", "o2") == LockStatus::kOk);
    BOOST_CHECK(gm.rw_read_release("never", "o") == LockStatus::kNotOwner);
}

// Rank misses: score/position of an unknown uid (or on an unknown board)
// report nullopt — the facade turns both into nil.
BOOST_AUTO_TEST_CASE(RankUnknownUidArms) {
    GlobalManager gm(default_config());
    gm.rank_update("b", "a", 1.0);
    BOOST_CHECK(!gm.rank_score("b", "ghost").has_value());
    BOOST_CHECK(!gm.rank_position("b", "ghost").has_value());
    BOOST_CHECK(!gm.rank_score("noboard", "a").has_value());
}

// Broadcast surfaces with unknown names/groups: since replays nothing
// and returns 0; commit on an unknown pair is a no-op.
BOOST_AUTO_TEST_CASE(BroadcastUnknownNameAndGroupArms) {
    GlobalManager gm(default_config());
    std::vector<std::string> rows;
    BOOST_CHECK_EQUAL(gm.broadcast_since("noboard", "g", &rows), 0u);
    BOOST_CHECK(rows.empty());
    gm.broadcast_configure("b", 10);
    BOOST_CHECK(gm.broadcast_push("b", "p1") > 0);
    BOOST_CHECK_EQUAL(gm.broadcast_since("b", "nogroup", &rows), 0u);
    gm.broadcast_commit("noboard", "g", 1);  // unknown name: no-op
    gm.broadcast_commit("b", "nogroup", 1);  // unknown group: no-op
}

// Reliable queue with an unknown delivery id: ack rejects, nack reports
// kNotFound (the facade's "not_found" string arm).
BOOST_AUTO_TEST_CASE(ReliableUnknownDeliveryArms) {
    GlobalManager gm(default_config());
    BOOST_CHECK(!gm.reliable_ack("q", 12345));
    BOOST_CHECK(gm.reliable_nack("q", 12345, 0) == NackResult::kNotFound);
    gm.reliable_push("q", "p");
    ReliableDelivery d;
    BOOST_REQUIRE(gm.reliable_pop("q", &d));
    BOOST_CHECK(!gm.reliable_ack("q", d.delivery_id + 999));
    BOOST_CHECK(gm.reliable_nack("q", d.delivery_id + 999, 0) ==
                NackResult::kNotFound);
}

// A scheduler ticking without a fire callback must survive: fire_task
// returns at the no-callback guard and the tick loop keeps counting
// attempts (run_count tallies fire attempts, not delivered callbacks).
BOOST_AUTO_TEST_CASE(TicksWithoutFireCallbackAreDropped) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);  // no set_task_fire_fn
    std::string error;
    BOOST_CHECK(gm.sched_register("interval", "nofn", "10", "svc", &error));
    gm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    gm.stop();
    auto info = gm.sched_get("nofn");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_GE(info->run_count, 1u);  // ticks ran, nothing delivered
    BOOST_CHECK(!info->paused);
    BOOST_CHECK(!info->done);
}

// A once task that fires through a live callback completes: done is set
// on the delivering tick and the task never fires again.
BOOST_AUTO_TEST_CASE(OnceTaskMarksDoneAfterFire) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    std::atomic<int> fires{0};
    gm.set_task_fire_fn([&fires](const std::string&, const std::string&) {
        ++fires;
        return true;
    });
    std::string error;
    BOOST_CHECK(gm.sched_register("once", "one", "20", "svc", &error));
    gm.start();
    BOOST_CHECK(wait_until([&] { return fires.load() >= 1; }, 2000));
    gm.stop();
    auto info = gm.sched_get("one");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK(info->done);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BOOST_CHECK_EQUAL(fires.load(), 1);
}

// Stopping while a fire callback is still running: the tick thread
// finishes the delivery, wraps to the loop head, and exits at the stop
// check there. (The check after the wait is the twin arm that catches a
// stop landing while the thread is parked in the wait.)
BOOST_AUTO_TEST_CASE(StopDuringFireCallbackExitsAtLoopTop) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    std::atomic<bool> in_callback{false};
    gm.set_task_fire_fn([&in_callback](const std::string&, const std::string&) {
        in_callback = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return true;
    });
    std::string error;
    BOOST_CHECK(gm.sched_register("interval", "slow", "10", "svc", &error));
    gm.start();
    BOOST_CHECK(wait_until([&] { return in_callback.load(); }, 2000));
    gm.stop();  // lands mid-callback; join waits for the loop-top exit
    auto info = gm.sched_get("slow");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK(!info->done);
}

// Resuming a paused cron task recomputes its next run from the cron
// expression instead of keeping the stale pre-pause schedule.
BOOST_AUTO_TEST_CASE(ResumeRearmsCronNextRun) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    std::string error;
    BOOST_CHECK(gm.sched_register("cron", "c", "* * * * *", "svc", &error));
    BOOST_CHECK(gm.sched_pause("c"));
    BOOST_CHECK(gm.sched_resume("c"));
    auto resumed = gm.sched_get("c");
    BOOST_REQUIRE(resumed.has_value());
    BOOST_CHECK(resumed->next_run_ms > GlobalManager::now_ms());
}

// Rate limiter tails: unknown limiter allow/remaining, the sliding
// window's age-out trim (on allow and on the query-only remaining
// copy), and a fresh fixed-window key reporting the full burst.
BOOST_AUTO_TEST_CASE(RateLimitWindowTails) {
    GlobalManager gm(default_config());
    // Unknown limiter: allow denies with a zeroed result, remaining is 0.
    auto none = gm.rate_limit_allow("nolimit", "k", 1);
    BOOST_CHECK(!none.allowed);
    BOOST_CHECK_EQUAL(gm.rate_limit_remaining("nolimit", "k"), 0.0);

    RateLimitConfig sliding;
    sliding.sliding = true;
    sliding.window_ms = 40;
    sliding.max_requests = 5;
    gm.rate_limit_configure("sl", sliding);
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK(gm.rate_limit_allow("sl", "u", 1).allowed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    // Aged hits trim on allow: the window is effectively empty again.
    auto trimmed = gm.rate_limit_allow("sl", "u", 1);
    BOOST_CHECK(trimmed.allowed);
    BOOST_CHECK_EQUAL(trimmed.remaining, 4.0);
    // Query-only path trims a copy (the stored window stays untouched).
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    BOOST_CHECK_EQUAL(gm.rate_limit_remaining("sl", "u"), 5.0);

    // Fixed window: a key with no bucket yet reports the full burst.
    RateLimitConfig fixed;
    fixed.rate = 10;
    fixed.burst = 4;
    gm.rate_limit_configure("fx", fixed);
    BOOST_CHECK_EQUAL(gm.rate_limit_remaining("fx", "fresh"), 4.0);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Branch closure: the remaining error/edge arms of every domain, each driven
// by a real two-step scenario (unknown id, expired window, ttl=0 shape,
// long payload, zero cost). No mocks, no injected clocks.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(BranchClosureSuite)

// cron rejection paths with a null error out-param, plus the character-class
// arms below '0' (a leading '-' in a step or a range bound) and the
// whitespace splitter's tab / collapsed-run arms.
BOOST_AUTO_TEST_CASE(CronParserEdgeArms) {
    CronFields fields;
    std::string error;
    BOOST_CHECK(!parse_cron("60 * * * *", &fields, nullptr));
    BOOST_CHECK(!parse_cron("* * * *", &fields, nullptr));
    // "*/-1": the step text starts below '0', so the numeric scan rejects it
    // through the first comparison rather than the second.
    BOOST_CHECK(!parse_cron("*/-1 * * * *", &fields, &error));
    BOOST_CHECK_NE(error.find("step is not a number"), std::string::npos);
    // "1--2": split at the first dash, so the high bound is "-2".
    BOOST_CHECK(!parse_cron("1--2 * * * *", &fields, &error));
    BOOST_CHECK_NE(error.find("bound is not a number"), std::string::npos);
    // Tabs separate fields and a whitespace run collapses: still 5 fields.
    BOOST_CHECK(parse_cron("\t*\t*\t*  \t*\t*", &fields, nullptr));
    // An explicit range (both bounds scanned) parses.
    BOOST_CHECK(parse_cron("0 0 31 * 1", &fields, &error));
}

// Both day sides restricted and the dom side not matching: the dow
// operand decides the match (the standard-cron OR fallback).
BOOST_AUTO_TEST_CASE(CronDomRestrictedFallsBackToDow) {
    CronFields fields;
    // 2026-09-14 00:00:00 UTC is a Monday (dow = 1).
    const std::uint64_t monday = 1789344000000ULL;
    BOOST_REQUIRE(parse_cron("0 0 31 * 1", &fields, nullptr));
    // From Monday 01:00 the dom (31st) never matches, so the next hit is the
    // following Monday.
    BOOST_CHECK_EQUAL(cron_next(fields, monday + kHour), monday + 7 * kDay);
}

// Both day sides restricted and the first candidate that lands on the 31st
// needs no dow rescue: the dom operand short-circuits the OR. The start is
// placed right after a Monday (Oct 28 2026) so no Monday precedes the
// 31st — the Oct 31 hit decides via dom_ok alone.
BOOST_AUTO_TEST_CASE(CronDomRestrictedDomMatchesShortCircuits) {
    CronFields fields;
    const std::uint64_t monday = 1789344000000ULL;  // 2026-09-14, Monday.
    BOOST_REQUIRE(parse_cron("0 0 31 * 1", &fields, nullptr));
    const std::uint64_t wed = monday + 44 * kDay;  // 2026-10-28, Wednesday.
    BOOST_CHECK_EQUAL(cron_next(fields, wed), wed + 3 * kDay);
}

BOOST_AUTO_TEST_CASE(ValidateWithNullErrorPointers) {
    GlobalConfig zero_cache;
    zero_cache.cache_max_size = 0;
    BOOST_CHECK(!validate_global_config(zero_cache, nullptr));
    GlobalConfig tiny_tick;
    tiny_tick.scheduler_tick_ms = 9;
    BOOST_CHECK(!validate_global_config(tiny_tick, nullptr));
    BOOST_CHECK(validate_global_config(default_config(), nullptr));
}

BOOST_AUTO_TEST_CASE(StopWithoutStartIsANoOp) {
    GlobalManager gm(default_config());
    // stop() on a manager whose tick thread never started.
    gm.stop();
    BOOST_CHECK_EQUAL(gm.sched_active_count(), 0u);
}

// data_incr_by across all three shapes: a key with no expiry stamp, a key
// with a live expiry, and a key holding a non-integer value; with and
// without the error out-param.
BOOST_AUTO_TEST_CASE(DataIncrEdgeArms) {
    GlobalManager gm(default_config());
    gm.data_set("forever", "10", 0);    // no expiry stamp at all
    gm.data_set("later", "20", 60000);  // live expiry stamp
    gm.data_set("text", "abc", 0);      // unparsable value

    std::int64_t n = 0;
    BOOST_CHECK(gm.data_incr_by("later", 1, &n, nullptr));  // live stamp
    BOOST_CHECK_EQUAL(n, 21);
    BOOST_CHECK(gm.data_incr_by("forever", 1, &n, nullptr));  // no stamp
    BOOST_CHECK_EQUAL(n, 11);
    // Unparsable payload: rejected, with and without an error string.
    BOOST_CHECK(!gm.data_incr_by("text", 1, &n, nullptr));
    std::string error;
    BOOST_CHECK(!gm.data_incr_by("text", 1, &n, &error));
    BOOST_CHECK_NE(error.find("not an integer"), std::string::npos);
    // Null out-param on a healthy increment.
    BOOST_CHECK(gm.data_incr_by("forever", 2, nullptr, nullptr));
    std::string value;
    BOOST_REQUIRE(gm.data_get("forever", &value));
    BOOST_CHECK_EQUAL(value, "13");
}

// mset rejects an empty key with a null error string; cache reads run
// without an out-param on both the miss-fill and the hit path; invalidate
// of an uncached key is a silent no-op.
BOOST_AUTO_TEST_CASE(MsetAndCacheOutParamArms) {
    GlobalManager gm(default_config());
    BOOST_CHECK(!gm.data_mset({{"", "v"}}, 0, nullptr));
    BOOST_CHECK_EQUAL(gm.data_size(), 0u);
    std::string error;
    BOOST_CHECK(!gm.data_mset({{"ok", "v"}, {"", "w"}}, 0, &error));
    BOOST_CHECK_NE(error.find("must not be empty"), std::string::npos);

    gm.data_set("c", "cached", 0);
    BOOST_CHECK(gm.cache_get("c", 1000, nullptr));  // miss-fill, no out
    BOOST_CHECK(gm.cache_get("c", 1000, nullptr));  // hit, no out
    gm.cache_invalidate("ghost");                   // not cached: a no-op
    BOOST_CHECK_EQUAL(gm.cache_size(), 1u);
    gm.cache_invalidate("c");
    BOOST_CHECK_EQUAL(gm.cache_size(), 0u);
}

// Exclusive locks: the stale-holder reclaim path with and without a TTL,
// extend's three rejection reasons, the spinlock registry select, an
// unknown name, and an expired TTL reported as zero remaining.
BOOST_AUTO_TEST_CASE(MutexTtlArms) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.mutex_acquire("default", "m", "o", 30) == LockStatus::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // The stale holder lost the lock; re-acquiring without a TTL stores 0.
    BOOST_CHECK(gm.mutex_acquire("default", "m", "o2", 0) == LockStatus::kOk);
    auto info = gm.mutex_info("default", "m");
    BOOST_REQUIRE(info.exists);
    BOOST_CHECK_EQUAL(info.ttl_remaining_ms, 0u);
    BOOST_CHECK_EQUAL(info.owner, "o2");

    // extend: unknown name, foreign owner, and an expired lock.
    BOOST_CHECK(!gm.mutex_extend("default", "ghost", "o", 1000));
    BOOST_CHECK(!gm.mutex_extend("default", "m", "other", 1000));
    gm.mutex_release("default", "m", "o2");
    BOOST_CHECK(gm.mutex_acquire("default", "exp", "o", 20) == LockStatus::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOST_CHECK(!gm.mutex_extend("default", "exp", "o", 1000));

    // The spinlock registry is a separate table selected by name.
    BOOST_CHECK(gm.mutex_acquire("spinlock", "m", "o", 0) == LockStatus::kOk);
    BOOST_CHECK_EQUAL(gm.mutex_registry_size("spinlock"), 1u);
    BOOST_CHECK(gm.mutex_info("spinlock", "m").exists);
    // No TTL on the live lock: the remaining TTL stays 0.
    BOOST_CHECK_EQUAL(gm.mutex_info("spinlock", "m").ttl_remaining_ms, 0u);
    BOOST_CHECK(gm.mutex_extend("spinlock", "m", "o", 5000));
    BOOST_CHECK(gm.mutex_info("spinlock", "m").ttl_remaining_ms > 0);
    BOOST_CHECK(!gm.mutex_info("default", "ghost").exists);

    // Extend stamps a fresh TTL, and drops it when asked for no expiry.
    BOOST_CHECK(gm.mutex_acquire("default", "m2", "x", 0) == LockStatus::kOk);
    BOOST_CHECK(gm.mutex_extend("default", "m2", "x", 1000));
    BOOST_CHECK(gm.mutex_info("default", "m2").ttl_remaining_ms > 0);
    BOOST_CHECK(gm.mutex_extend("default", "m2", "x", 0));
    BOOST_CHECK_EQUAL(gm.mutex_info("default", "m2").ttl_remaining_ms, 0u);
}

// The same TTL shapes on the rwlock registry, plus the unknown-name and
// no-expiry info arms.
BOOST_AUTO_TEST_CASE(RwLockTtlArms) {
    GlobalManager gm(default_config());
    BOOST_CHECK(gm.rw_write_acquire("w", "o", 20) == LockStatus::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // The stale writer is reclaimed with no expiry stamped.
    BOOST_CHECK(gm.rw_write_acquire("w", "o2", 0) == LockStatus::kOk);
    auto info = gm.rwlock_info("w");
    BOOST_REQUIRE(info.exists);
    BOOST_CHECK_EQUAL(info.write_owner, "o2");
    BOOST_CHECK_EQUAL(info.write_ttl_remaining_ms, 0u);

    // extend: unknown lock, foreign owner, then a live lock (with and
    // without a TTL stamp).
    BOOST_CHECK(!gm.rw_write_extend("ghost", "o", 1000));
    BOOST_CHECK(!gm.rw_write_extend("w", "other", 1000));
    BOOST_CHECK(gm.rw_write_extend("w", "o2", 1000));
    BOOST_CHECK(gm.rwlock_info("w").write_ttl_remaining_ms > 0);
    BOOST_CHECK(gm.rw_write_extend("w", "o2", 0));
    BOOST_CHECK_EQUAL(gm.rwlock_info("w").write_ttl_remaining_ms, 0u);
    // An expired write lock reports zero remaining TTL.
    BOOST_CHECK(gm.rw_write_acquire("short", "o", 20) == LockStatus::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOST_CHECK_EQUAL(gm.rwlock_info("short").write_ttl_remaining_ms, 0u);
    BOOST_CHECK(!gm.rwlock_info("ghost").exists);
    BOOST_CHECK(gm.rw_write_release("ghost", "o") == LockStatus::kNotOwner);
}

// Leaderboard edges: a top window wider than the board, from=0 and an
// inverted range on a known board, unknown boards for count/remove, and a
// heap-length uid (long enough to leave the small-string buffer).
BOOST_AUTO_TEST_CASE(RankEdgeArms) {
    GlobalManager gm(default_config());
    gm.rank_update("b", "alice", 30.0);
    gm.rank_update("b", "a-very-long-identifier", 20.0);
    // A top window at least as wide as the board needs no resize.
    BOOST_CHECK_EQUAL(gm.rank_top("b", 10).size(), 2u);
    BOOST_CHECK_EQUAL(gm.rank_range("b", 0, 5).size(), 0u);
    BOOST_CHECK_EQUAL(gm.rank_range("b", 3, 1).size(), 0u);
    BOOST_CHECK_EQUAL(gm.rank_range("b", 1, 5).size(), 2u);
    BOOST_CHECK_EQUAL(gm.rank_count("nobody"), 0u);
    BOOST_CHECK(!gm.rank_remove("nobody", "alice"));
    BOOST_CHECK_EQUAL(gm.rank_score("nobody", "alice").has_value(), false);
    // Around a long uid that is absent from the board: no target.
    auto around = gm.rank_around("b", "nobody-here", 1);
    BOOST_CHECK(!around.target.has_value());
    auto found = gm.rank_around("b", "a-very-long-identifier", 1);
    BOOST_REQUIRE(found.target.has_value());
    BOOST_CHECK_EQUAL(found.target->score, 20.0);
}

// Queue pops and delay-queue reads without an out-param, plus a delay
// queue whose head is still in the future.
BOOST_AUTO_TEST_CASE(QueueAndDelayArms) {
    GlobalManager gm(default_config());
    gm.queue_push("q", "payload");
    BOOST_CHECK(gm.queue_pop("q", nullptr));  // discards the value
    BOOST_CHECK(!gm.queue_pop("q", nullptr));

    gm.priority_push("p", "low", 5);
    BOOST_CHECK(gm.priority_pop("p", nullptr));
    gm.priority_push("p", "high", 1);
    std::string value;
    BOOST_REQUIRE(gm.priority_pop("p", &value));
    BOOST_CHECK_EQUAL(value, "high");

    // Head not due yet: neither the pop nor the ready counter sees it.
    gm.delay_push("d", "later", 60000);
    BOOST_CHECK(!gm.delay_pop("d", nullptr));
    BOOST_CHECK_EQUAL(gm.delay_ready("d"), 0u);
    BOOST_CHECK_EQUAL(gm.delay_pending("d"), 1u);
    // A ready item pops without an out-param.
    gm.delay_push_at("d", "now", GlobalManager::now_ms());
    BOOST_CHECK(gm.delay_pop("d", nullptr));
    BOOST_CHECK(!gm.delay_pop("d", nullptr));
    BOOST_CHECK_EQUAL(gm.delay_pending("ghost"), 0u);
}

// Broadcast cursors: peeking without a sink and committing an older
// sequence number (the cursor never moves backwards).
BOOST_AUTO_TEST_CASE(BroadcastCursorArms) {
    GlobalManager gm(default_config());
    gm.broadcast_push("b", "one");
    const std::uint64_t second = gm.broadcast_push("b", "two");
    // Attaching starts the group at the queue head (last assigned seq), so
    // a null sink reports nothing new.
    BOOST_CHECK_EQUAL(gm.broadcast_attach("b", "g"), second);
    BOOST_CHECK_EQUAL(gm.broadcast_since("b", "g", nullptr), 0u);
    // A stale commit never moves the cursor backwards, a newer one does.
    gm.broadcast_commit("b", "g", 1);
    BOOST_CHECK_EQUAL(gm.broadcast_since("b", "g", nullptr), 0u);
    gm.broadcast_commit("b", "g", 5);
    BOOST_CHECK_EQUAL(gm.broadcast_since("b", "g", nullptr), 0u);
    // A group created by commit replays everything past its cursor, and
    // skips what it already consumed.
    gm.broadcast_commit("b", "late", 0);
    std::vector<std::string> replayed;
    BOOST_CHECK_EQUAL(gm.broadcast_since("b", "late", &replayed), second);
    BOOST_REQUIRE_EQUAL(replayed.size(), 2u);
    BOOST_CHECK_EQUAL(replayed[0], "one");
    gm.broadcast_commit("b", "mid", 1);
    std::vector<std::string> partial;
    BOOST_CHECK_EQUAL(gm.broadcast_since("b", "mid", &partial), second);
    BOOST_REQUIRE_EQUAL(partial.size(), 1u);
    BOOST_CHECK_EQUAL(partial[0], "two");
}

// Reliable queues: a long payload through pop (heap path) with and without
// an out-param, dead-letter paging for an unknown queue, purging a queue
// that does not exist, and the inflight/dead counters of a foreign name.
BOOST_AUTO_TEST_CASE(ReliableQueueArms) {
    GlobalManager gm(default_config());
    const std::string payload = "a-payload-far-longer-than-the-inline-buffer";
    gm.reliable_push("r", payload);
    BOOST_CHECK(gm.reliable_pop("r", nullptr));
    BOOST_CHECK_EQUAL(gm.reliable_inflight("r"), 1u);

    gm.reliable_configure("d", 1);
    gm.reliable_push("d", payload);
    ReliableDelivery delivery;
    BOOST_REQUIRE(gm.reliable_pop("d", &delivery));
    BOOST_CHECK_EQUAL(delivery.payload, payload);
    BOOST_CHECK(gm.reliable_nack("d", delivery.delivery_id, 0) ==
                NackResult::kDead);
    BOOST_CHECK_EQUAL(gm.reliable_dead_size("d"), 1u);
    auto dead = gm.reliable_dead_range("d", 0, 10);
    BOOST_REQUIRE_EQUAL(dead.size(), 1u);
    BOOST_CHECK_EQUAL(dead[0].payload, payload);
    // Foreign names report empty everywhere and purge is a silent no-op.
    BOOST_CHECK_EQUAL(gm.reliable_dead_size("ghost"), 0u);
    BOOST_CHECK(gm.reliable_dead_range("ghost", 0, 10).empty());
    BOOST_CHECK_EQUAL(gm.reliable_inflight("ghost"), 0u);
    gm.reliable_dead_purge("ghost");
    gm.reliable_dead_purge("d");
    BOOST_CHECK_EQUAL(gm.reliable_dead_size("d"), 0u);
}

// Scheduler registration failures with a null error string: an
// unmatchable cron, a malformed interval amount, and a duplicate name.
BOOST_AUTO_TEST_CASE(SchedRegisterFailureArms) {
    GlobalManager gm(default_config());
    BOOST_CHECK(!gm.sched_register("cron", "t", "0 0 31 2 *", "svc", nullptr));
    BOOST_CHECK(!gm.sched_register("interval", "t", "abc", "svc", nullptr));
    BOOST_CHECK(!gm.sched_register("interval", "t", "", "svc", nullptr));
    BOOST_CHECK(gm.sched_register("interval", "t", "100", "svc", nullptr));
    BOOST_CHECK(!gm.sched_register("interval", "t", "100", "svc", nullptr));
    BOOST_CHECK_EQUAL(gm.sched_list().size(), 1u);
    BOOST_CHECK(gm.sched_remove("t"));
}

// Resume arms: a live task (nothing to re-arm), a paused interval task
// (re-armed from now), and a paused once-task that already ran (nothing to
// re-arm). sched_active_count counts only neither-paused-nor-done tasks.
BOOST_AUTO_TEST_CASE(SchedPauseResumeArms) {
    GlobalManager gm(default_config());
    BOOST_REQUIRE(
        gm.sched_register("interval", "live", "60000", "svc", nullptr));
    BOOST_REQUIRE(
        gm.sched_register("interval", "paused", "60000", "svc", nullptr));
    BOOST_REQUIRE(gm.sched_register("once", "done", "10", "svc", nullptr));
    BOOST_CHECK(!gm.sched_pause("ghost"));
    BOOST_CHECK(!gm.sched_resume("ghost"));

    gm.set_task_fire_fn(
        [](const std::string&, const std::string&) { return true; });
    gm.start();
    BOOST_CHECK(wait_until([&] { return gm.sched_get("done")->done; }, 500));
    gm.stop();

    // A live (never paused) task resumes without re-arming.
    auto live = gm.sched_get("live");
    BOOST_REQUIRE(live.has_value());
    BOOST_CHECK(gm.sched_resume("live"));
    BOOST_CHECK_EQUAL(gm.sched_get("live")->next_run_ms, live->next_run_ms);

    // A paused interval task re-arms from now.
    BOOST_CHECK(gm.sched_pause("paused"));
    BOOST_CHECK_EQUAL(gm.sched_active_count(), 1u);  // live only
    BOOST_CHECK(gm.sched_resume("paused"));
    BOOST_CHECK(gm.sched_get("paused")->next_run_ms >= GlobalManager::now_ms());
    BOOST_CHECK_EQUAL(gm.sched_active_count(), 2u);  // live + paused again

    // A paused once-task that already ran keeps its cleared next_run.
    BOOST_CHECK(gm.sched_pause("done"));
    BOOST_CHECK(gm.sched_resume("done"));
    BOOST_CHECK_EQUAL(gm.sched_get("done")->next_run_ms, 0u);
    BOOST_CHECK_EQUAL(gm.sched_active_count(), 2u);
}

// Rate limiter arms: a zero-cost request, a zero refill rate (no retry
// hint), and a query for a key that has no bucket yet.
BOOST_AUTO_TEST_CASE(RateLimitCostArms) {
    GlobalManager gm(default_config());
    RateLimitConfig cfg;
    cfg.rate = 1000.0;
    cfg.burst = 2.0;
    gm.rate_limit_configure("c", cfg);
    // cost == 0 skips the consume branch and reports the full bucket.
    auto zero = gm.rate_limit_allow("c", "k", 0.0);
    BOOST_CHECK(!zero.allowed);
    BOOST_CHECK_EQUAL(zero.remaining, 2.0);
    BOOST_CHECK_EQUAL(zero.retry_after_ms, 0u);
    BOOST_CHECK(gm.rate_limit_allow("c", "k", 1.0).allowed);
    BOOST_CHECK(gm.rate_limit_allow("c", "k", 1.0).allowed);
    // Drained bucket with a zero refill rate: denied, no retry hint.
    RateLimitConfig frozen;
    frozen.rate = 0.0;
    frozen.burst = 1.0;
    gm.rate_limit_configure("z", frozen);
    BOOST_CHECK(gm.rate_limit_allow("z", "k", 1.0).allowed);
    auto denied = gm.rate_limit_allow("z", "k", 1.0);
    BOOST_CHECK(!denied.allowed);
    BOOST_CHECK_EQUAL(denied.retry_after_ms, 0u);
    // A configured limiter with an unknown key still reports the burst.
    BOOST_CHECK_EQUAL(gm.rate_limit_remaining("c", "never-used"), 2.0);
}

// TTL=0 arms across data, cache, mutex, rwlock: the ternary branches
// that stamp expire_at_ms = 0 vs now + ttl_ms.
BOOST_AUTO_TEST_CASE(TtlZeroArms) {
    GlobalManager gm(default_config());

    // data_set with ttl=0 stores no expiry.
    gm.data_set("k", "v", 0);
    std::string value;
    BOOST_CHECK(gm.data_get("k", &value));
    BOOST_CHECK_EQUAL(value, "v");

    // cache_get with effective_ttl=0 records no expiry (config default).
    GlobalConfig no_ttl_cfg;
    no_ttl_cfg.cache_default_ttl_ms = 0;
    GlobalManager no_ttl(no_ttl_cfg);
    no_ttl.data_set("nk", "x", 0);
    BOOST_CHECK(no_ttl.cache_get("nk", 0, &value));
    BOOST_CHECK_EQUAL(value, "x");
    // Hit path with default ttl=0 never expires.
    BOOST_CHECK(no_ttl.cache_get("nk", 0, &value));

    // mutex_acquire with ttl=0: expire_at_ms = 0 (no expiry).
    BOOST_CHECK(gm.mutex_acquire("mutex", "m1", "o", 0) == LockStatus::kOk);
    auto info = gm.mutex_info("mutex", "m1");
    BOOST_REQUIRE(info.exists);
    BOOST_CHECK_EQUAL(info.ttl_remaining_ms, 0u);
    // extend with ttl=0 drops the TTL.
    BOOST_CHECK(gm.mutex_extend("mutex", "m1", "o", 1000));
    BOOST_CHECK(gm.mutex_info("mutex", "m1").ttl_remaining_ms > 0);
    BOOST_CHECK(gm.mutex_extend("mutex", "m1", "o", 0));
    BOOST_CHECK_EQUAL(gm.mutex_info("mutex", "m1").ttl_remaining_ms, 0u);

    // rw_write_acquire with ttl=0: write_expire_at_ms = 0.
    BOOST_CHECK(gm.rw_write_acquire("rw1", "o", 0) == LockStatus::kOk);
    auto rw_info = gm.rwlock_info("rw1");
    BOOST_REQUIRE(rw_info.exists);
    BOOST_CHECK_EQUAL(rw_info.write_ttl_remaining_ms, 0u);
    // rw_write_extend with ttl=0 drops the TTL.
    BOOST_CHECK(gm.rw_write_extend("rw1", "o", 1000));
    BOOST_CHECK(gm.rwlock_info("rw1").write_ttl_remaining_ms > 0);
    BOOST_CHECK(gm.rw_write_extend("rw1", "o", 0));
    BOOST_CHECK_EQUAL(gm.rwlock_info("rw1").write_ttl_remaining_ms, 0u);
}

// Null error out-params on sched_register rejection paths.
BOOST_AUTO_TEST_CASE(SchedRegisterNullErrorArms) {
    GlobalManager gm(default_config());
    // Unmatchable cron with null error.
    BOOST_CHECK(!gm.sched_register("cron", "t1", "0 0 31 2 *", "svc", nullptr));
    // Malformed interval with null error.
    BOOST_CHECK(!gm.sched_register("interval", "t2", "abc", "svc", nullptr));
    // Empty interval with null error.
    BOOST_CHECK(!gm.sched_register("interval", "t3", "", "svc", nullptr));
    // Duplicate name with null error.
    BOOST_CHECK(gm.sched_register("interval", "t4", "100", "svc", nullptr));
    BOOST_CHECK(!gm.sched_register("interval", "t4", "100", "svc", nullptr));
    BOOST_CHECK(gm.sched_remove("t4"));
}

// fire_task with null callback (no set_task_fire_fn) returns early.
BOOST_AUTO_TEST_CASE(FireTaskNullCallback) {
    // The default tick interval is 250ms; drive the loop at 10ms so due
    // ticks arrive quickly once the loop is scheduled.
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);  // no set_task_fire_fn
    std::string error;
    BOOST_CHECK(gm.sched_register("interval", "t", "10", "svc", &error));
    gm.start();
    // The tick loop runs but fire_task returns at the null-callback guard.
    // Poll instead of a fixed sleep: on a loaded box the fresh tick thread
    // can stay unscheduled far past any fixed window, while run_count
    // increments on the first due tick the loop does get.
    BOOST_CHECK(wait_until(
        [&] {
            auto live = gm.sched_get("t");
            return live.has_value() && live->run_count >= 1u;
        },
        3000));
    gm.stop();
    auto info = gm.sched_get("t");
    BOOST_REQUIRE(info.has_value());
    // run_count increments on every tick attempt even without callback.
    BOOST_CHECK_GE(info->run_count, 1u);
    BOOST_CHECK(!info->paused);
    BOOST_CHECK(!info->done);
}

// Scheduler tick_loop: the stop check at loop top and after wait both fire.
BOOST_AUTO_TEST_CASE(TickLoopStopChecks) {
    GlobalConfig config;
    config.scheduler_tick_ms = 10;
    GlobalManager gm(config);
    gm.set_task_fire_fn(
        [](const std::string&, const std::string&) { return true; });
    std::string error;
    BOOST_CHECK(gm.sched_register("interval", "t", "1000000", "svc", &error));
    gm.start();
    // Stop immediately: the thread is either at loop top or in the wait.
    gm.stop();
    // The task remains registered and not done.
    BOOST_CHECK(gm.sched_get("t").has_value());
    auto info = gm.sched_get("t");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK(!info->done);
    BOOST_CHECK(!info->paused);
}

// cron_next retire arm (no match within horizon) is already excluded in
// source; here we just verify the ternary at line 179 (day_matches OR).
BOOST_AUTO_TEST_CASE(CronDayMatchesOrFallback) {
    CronFields f;
    // Both sides restricted, OR fallback: match on dow when dom doesn't.
    BOOST_REQUIRE(parse_cron("0 0 31 * 1", &f, nullptr));
    // 2026-09-14 00:00:00 UTC is a Monday (dow = 1).
    const std::uint64_t monday = 1789344000000ULL;
    // From Monday 01:00 the dom (31st) never matches, so the next hit is the
    // following Monday (dow side).
    BOOST_CHECK_EQUAL(cron_next(f, monday + kHour), monday + 7 * kDay);
}

BOOST_AUTO_TEST_SUITE_END()
