#define BOOST_TEST_MODULE QueryCacheTest
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <thread>

#include "shield/data/query_cache.hpp"

using namespace shield::data;
using namespace shield::data::cache;

BOOST_AUTO_TEST_SUITE(CacheConfigTests)

BOOST_AUTO_TEST_CASE(TestCacheConfigDefaults) {
    CacheConfig config;
    BOOST_CHECK_EQUAL(config.max_entries, 1000u);
    BOOST_CHECK_EQUAL(config.default_ttl.count(), 300);
    BOOST_CHECK_EQUAL(config.cleanup_interval.count(), 60);
    BOOST_CHECK(config.enable_statistics);
    BOOST_CHECK(config.enable_async_refresh);
    BOOST_CHECK_CLOSE(config.hit_ratio_threshold, 0.8, 0.01);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CacheKeyTests)

BOOST_AUTO_TEST_CASE(TestCacheKeyFromNativeQuery) {
    std::vector<DataValue> params;
    CacheKey key("users", "SELECT * FROM users", params);
    BOOST_CHECK(!key.to_string().empty());
    BOOST_CHECK(key.to_string().find("users:") == 0);
}

BOOST_AUTO_TEST_CASE(TestCacheKeyEquality) {
    std::vector<DataValue> params;
    CacheKey key1("users", "SELECT * FROM users", params);
    CacheKey key2("users", "SELECT * FROM users", params);
    BOOST_CHECK(key1 == key2);
}

BOOST_AUTO_TEST_CASE(TestCacheKeyInequalityDifferentCollection) {
    std::vector<DataValue> params;
    CacheKey key1("users", "SELECT * FROM users", params);
    CacheKey key2("orders", "SELECT * FROM users", params);
    BOOST_CHECK(!(key1 == key2));
}

BOOST_AUTO_TEST_CASE(TestCacheKeyInequalityDifferentQuery) {
    std::vector<DataValue> params;
    CacheKey key1("users", "SELECT * FROM users", params);
    CacheKey key2("users", "SELECT * FROM orders", params);
    BOOST_CHECK(!(key1 == key2));
}

BOOST_AUTO_TEST_CASE(TestCacheKeyLessThan) {
    std::vector<DataValue> params;
    CacheKey key1("aaa", "query1", params);
    CacheKey key2("bbb", "query1", params);
    BOOST_CHECK(key1 < key2);
}

BOOST_AUTO_TEST_CASE(TestCacheKeyWithParams) {
    std::vector<DataValue> params1 = {DataValue(std::string("alice"))};
    std::vector<DataValue> params2 = {DataValue(std::string("bob"))};
    CacheKey key1("users", "SELECT * FROM users WHERE name = ?", params1);
    CacheKey key2("users", "SELECT * FROM users WHERE name = ?", params2);
    BOOST_CHECK(!(key1 == key2));
}

BOOST_AUTO_TEST_CASE(TestCacheKeyHash) {
    std::vector<DataValue> params;
    CacheKey key("users", "SELECT * FROM users", params);
    CacheKeyHash hasher;
    std::size_t h = hasher(key);
    BOOST_CHECK(h != 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CacheEntryTests)

BOOST_AUTO_TEST_CASE(TestCacheEntryCreation) {
    QueryResult result;
    result.success = true;
    CacheEntry entry(result, std::chrono::seconds{60});
    BOOST_CHECK(!entry.is_expired());
    BOOST_CHECK_EQUAL(entry.get_access_count(), 0u);
    BOOST_CHECK_EQUAL(entry.get_ttl().count(), 60);
}

BOOST_AUTO_TEST_CASE(TestCacheEntryAccess) {
    QueryResult result;
    result.success = true;
    CacheEntry entry(result, std::chrono::seconds{60});

    const auto& r = entry.get_result();
    BOOST_CHECK(r.success);
    BOOST_CHECK_EQUAL(entry.get_access_count(), 1u);

    entry.get_result();
    BOOST_CHECK_EQUAL(entry.get_access_count(), 2u);
}

BOOST_AUTO_TEST_CASE(TestCacheEntryNeverExpiresWithZeroTtl) {
    QueryResult result;
    result.success = true;
    CacheEntry entry(result, std::chrono::seconds{0});
    BOOST_CHECK(!entry.is_expired());
}

BOOST_AUTO_TEST_CASE(TestCacheEntryExpiresAfterTtl) {
    QueryResult result;
    result.success = true;
    CacheEntry entry(result, std::chrono::seconds{1});
    BOOST_CHECK(!entry.is_expired());

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    BOOST_CHECK(entry.is_expired());
}

BOOST_AUTO_TEST_CASE(TestCacheEntryAge) {
    QueryResult result;
    result.success = true;
    CacheEntry entry(result, std::chrono::seconds{60});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto age = entry.get_age();
    BOOST_CHECK(age.count() >= 40);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LRUCacheTests)

BOOST_AUTO_TEST_CASE(TestLRUCachePutAndGet) {
    LRUCache<int, int> cache(3);
    cache.put(1, 10);
    cache.put(2, 20);

    auto val = cache.get(1);
    BOOST_CHECK(val.has_value());
    BOOST_CHECK_EQUAL(*val, 10);

    val = cache.get(2);
    BOOST_CHECK(val.has_value());
    BOOST_CHECK_EQUAL(*val, 20);
}

BOOST_AUTO_TEST_CASE(TestLRUCacheMiss) {
    LRUCache<int, int> cache(3);
    auto val = cache.get(99);
    BOOST_CHECK(!val.has_value());
}

BOOST_AUTO_TEST_CASE(TestLRUCacheEviction) {
    LRUCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);  // evicts key 1

    auto val = cache.get(1);
    BOOST_CHECK(!val.has_value());

    val = cache.get(2);
    BOOST_CHECK(val.has_value());

    val = cache.get(3);
    BOOST_CHECK(val.has_value());
}

BOOST_AUTO_TEST_CASE(TestLRUCacheUpdate) {
    LRUCache<int, int> cache(2);
    cache.put(1, 10);
    cache.put(1, 99);

    auto val = cache.get(1);
    BOOST_CHECK(val.has_value());
    BOOST_CHECK_EQUAL(*val, 99);
    BOOST_CHECK_EQUAL(cache.size(), 1u);
}

BOOST_AUTO_TEST_CASE(TestLRUCacheRemove) {
    LRUCache<int, int> cache(3);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.remove(1);

    BOOST_CHECK(!cache.get(1).has_value());
    BOOST_CHECK_EQUAL(cache.size(), 1u);
}

BOOST_AUTO_TEST_CASE(TestLRUCacheClear) {
    LRUCache<int, int> cache(3);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.clear();

    BOOST_CHECK_EQUAL(cache.size(), 0u);
    BOOST_CHECK(!cache.get(1).has_value());
}

BOOST_AUTO_TEST_CASE(TestLRUCacheGetAllKeys) {
    LRUCache<int, int> cache(3);
    cache.put(1, 10);
    cache.put(2, 20);
    cache.put(3, 30);

    auto keys = cache.get_all_keys();
    BOOST_CHECK_EQUAL(keys.size(), 3u);
}

BOOST_AUTO_TEST_CASE(TestLRUCacheSize) {
    LRUCache<int, int> cache(5);
    BOOST_CHECK_EQUAL(cache.size(), 0u);
    cache.put(1, 10);
    BOOST_CHECK_EQUAL(cache.size(), 1u);
    cache.put(2, 20);
    BOOST_CHECK_EQUAL(cache.size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CacheStatisticsTests)

BOOST_AUTO_TEST_CASE(TestCacheStatisticsDefaults) {
    CacheStatistics stats;
    BOOST_CHECK_EQUAL(stats.total_requests.load(), 0u);
    BOOST_CHECK_EQUAL(stats.cache_hits.load(), 0u);
    BOOST_CHECK_EQUAL(stats.cache_misses.load(), 0u);
    BOOST_CHECK_EQUAL(stats.cache_evictions.load(), 0u);
    BOOST_CHECK_CLOSE(stats.get_hit_ratio(), 0.0, 0.01);
    BOOST_CHECK_CLOSE(stats.get_miss_ratio(), 1.0, 0.01);
}

BOOST_AUTO_TEST_CASE(TestCacheStatisticsHitRatio) {
    CacheStatistics stats;
    stats.total_requests = 10;
    stats.cache_hits = 7;
    BOOST_CHECK_CLOSE(stats.get_hit_ratio(), 0.7, 0.01);
    BOOST_CHECK_CLOSE(stats.get_miss_ratio(), 0.3, 0.01);
}

BOOST_AUTO_TEST_CASE(TestCacheStatisticsReset) {
    CacheStatistics stats;
    stats.total_requests = 100;
    stats.cache_hits = 50;
    stats.reset();
    BOOST_CHECK_EQUAL(stats.total_requests.load(), 0u);
    BOOST_CHECK_EQUAL(stats.cache_hits.load(), 0u);
}

BOOST_AUTO_TEST_CASE(TestCacheStatisticsUptime) {
    CacheStatistics stats;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto uptime = stats.get_uptime();
    BOOST_CHECK(uptime.count() >= 40);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(QueryCacheManagerTests)

BOOST_AUTO_TEST_CASE(TestCacheManagerPutAndGet) {
    CacheConfig config;
    config.max_entries = 10;
    config.default_ttl = std::chrono::seconds{60};
    QueryCacheManager manager(config);

    std::vector<DataValue> params;
    CacheKey key("test", "SELECT 1", params);

    QueryResult result;
    result.success = true;
    manager.put(key, result);

    auto cached = manager.get(key);
    BOOST_CHECK(cached.has_value());
    BOOST_CHECK(cached->success);
}

BOOST_AUTO_TEST_CASE(TestCacheManagerMiss) {
    CacheConfig config;
    config.max_entries = 10;
    QueryCacheManager manager(config);

    std::vector<DataValue> params;
    CacheKey key("test", "SELECT 1", params);

    auto cached = manager.get(key);
    BOOST_CHECK(!cached.has_value());
}

BOOST_AUTO_TEST_CASE(TestCacheManagerInvalidate) {
    CacheConfig config;
    config.max_entries = 10;
    QueryCacheManager manager(config);

    std::vector<DataValue> params;
    CacheKey key("test", "SELECT 1", params);

    QueryResult result;
    result.success = true;
    manager.put(key, result);

    manager.invalidate(key);
    BOOST_CHECK(!manager.get(key).has_value());
}

BOOST_AUTO_TEST_CASE(TestCacheManagerClear) {
    CacheConfig config;
    config.max_entries = 10;
    QueryCacheManager manager(config);

    std::vector<DataValue> params;
    CacheKey key1("test", "SELECT 1", params);
    CacheKey key2("test", "SELECT 2", params);

    QueryResult result;
    result.success = true;
    manager.put(key1, result);
    manager.put(key2, result);

    manager.clear();
    BOOST_CHECK(!manager.get(key1).has_value());
    BOOST_CHECK(!manager.get(key2).has_value());
}

BOOST_AUTO_TEST_CASE(TestCacheManagerStatistics) {
    CacheConfig config;
    config.max_entries = 10;
    QueryCacheManager manager(config);

    std::vector<DataValue> params;
    CacheKey key("test", "SELECT 1", params);

    // Miss
    manager.get(key);

    // Hit
    QueryResult result;
    result.success = true;
    manager.put(key, result);
    manager.get(key);

    auto stats = manager.get_statistics();
    BOOST_CHECK_EQUAL(stats.total_requests.load(), 2u);
    BOOST_CHECK_EQUAL(stats.cache_hits.load(), 1u);
    BOOST_CHECK_EQUAL(stats.cache_misses.load(), 1u);
}

BOOST_AUTO_TEST_CASE(TestCacheManagerInvalidateCollection) {
    CacheConfig config;
    config.max_entries = 10;
    QueryCacheManager manager(config);

    std::vector<DataValue> params;
    CacheKey key1("users", "SELECT * FROM users", params);
    CacheKey key2("orders", "SELECT * FROM orders", params);

    QueryResult result;
    result.success = true;
    manager.put(key1, result);
    manager.put(key2, result);

    manager.invalidate_collection("users");
    BOOST_CHECK(!manager.get(key1).has_value());
    BOOST_CHECK(manager.get(key2).has_value());
}

BOOST_AUTO_TEST_CASE(TestCacheManagerDefaultTtlApplied) {
    CacheConfig config;
    config.max_entries = 10;
    config.default_ttl = std::chrono::seconds{1};
    QueryCacheManager manager(config);

    std::vector<DataValue> params;
    CacheKey key("test", "SELECT 1", params);

    QueryResult result;
    result.success = true;
    manager.put(key, result);  // uses default ttl

    BOOST_CHECK(manager.get(key).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    BOOST_CHECK(!manager.get(key).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(QueryPerformanceMonitorTests)

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorRecord) {
    QueryPerformanceMonitor monitor;
    monitor.record_query_execution("SELECT * FROM users",
                                   std::chrono::milliseconds{10}, false);

    auto metrics = monitor.get_metrics("SELECT * FROM users");
    BOOST_CHECK_EQUAL(metrics.execution_count, 1u);
    BOOST_CHECK_EQUAL(metrics.cache_hit_count, 0u);
    BOOST_CHECK_EQUAL(metrics.avg_execution_time.count(), 10);
}

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorCacheHit) {
    QueryPerformanceMonitor monitor;
    monitor.record_query_execution("SELECT * FROM users",
                                   std::chrono::milliseconds{5}, true);

    auto metrics = monitor.get_metrics("SELECT * FROM users");
    BOOST_CHECK_EQUAL(metrics.cache_hit_count, 1u);
    BOOST_CHECK_CLOSE(metrics.cache_hit_ratio, 1.0, 0.01);
}

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorMultipleRecords) {
    QueryPerformanceMonitor monitor;
    monitor.record_query_execution("q1", std::chrono::milliseconds{10}, false);
    monitor.record_query_execution("q1", std::chrono::milliseconds{20}, true);
    monitor.record_query_execution("q1", std::chrono::milliseconds{30}, false);

    auto metrics = monitor.get_metrics("q1");
    BOOST_CHECK_EQUAL(metrics.execution_count, 3u);
    BOOST_CHECK_EQUAL(metrics.cache_hit_count, 1u);
    BOOST_CHECK_EQUAL(metrics.min_execution_time.count(), 10);
    BOOST_CHECK_EQUAL(metrics.max_execution_time.count(), 30);
}

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorUnknownSignature) {
    QueryPerformanceMonitor monitor;
    auto metrics = monitor.get_metrics("nonexistent");
    BOOST_CHECK_EQUAL(metrics.execution_count, 0u);
}

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorTopSlowQueries) {
    QueryPerformanceMonitor monitor;
    monitor.record_query_execution("fast", std::chrono::milliseconds{1}, false);
    monitor.record_query_execution("slow", std::chrono::milliseconds{100}, false);
    monitor.record_query_execution("medium", std::chrono::milliseconds{50}, false);

    auto top = monitor.get_top_slow_queries(2);
    BOOST_CHECK_EQUAL(top.size(), 2u);
    BOOST_CHECK_EQUAL(top[0].query_signature, "slow");
    BOOST_CHECK_EQUAL(top[1].query_signature, "medium");
}

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorMostFrequentQueries) {
    QueryPerformanceMonitor monitor;
    for (int i = 0; i < 10; i++) {
        monitor.record_query_execution("frequent", std::chrono::milliseconds{5},
                                      false);
    }
    monitor.record_query_execution("rare", std::chrono::milliseconds{5}, false);

    auto top = monitor.get_most_frequent_queries(1);
    BOOST_CHECK_EQUAL(top.size(), 1u);
    BOOST_CHECK_EQUAL(top[0].query_signature, "frequent");
    BOOST_CHECK_EQUAL(top[0].execution_count, 10u);
}

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorReset) {
    QueryPerformanceMonitor monitor;
    monitor.record_query_execution("q1", std::chrono::milliseconds{10}, false);
    monitor.reset_metrics();

    auto metrics = monitor.get_metrics("q1");
    BOOST_CHECK_EQUAL(metrics.execution_count, 0u);
}

BOOST_AUTO_TEST_CASE(TestPerformanceMonitorEnableDisable) {
    QueryPerformanceMonitor monitor;
    BOOST_CHECK(monitor.is_monitoring_enabled());

    monitor.disable_monitoring();
    BOOST_CHECK(!monitor.is_monitoring_enabled());

    // Recording while disabled should be a no-op
    monitor.record_query_execution("q1", std::chrono::milliseconds{10}, false);
    auto metrics = monitor.get_metrics("q1");
    BOOST_CHECK_EQUAL(metrics.execution_count, 0u);

    monitor.enable_monitoring();
    BOOST_CHECK(monitor.is_monitoring_enabled());
}

BOOST_AUTO_TEST_SUITE_END()
