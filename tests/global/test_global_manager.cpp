// GlobalManager unit tests (shield_global P0). Constructed directly from
// GlobalConfig: no global config store, no Lua, no bootstrap. Config
// parsing tests opt in via global_config().set().
#define BOOST_TEST_MODULE GlobalManagerTests
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
    gm.stop();
    gm.stop();
}

BOOST_AUTO_TEST_SUITE_END()
