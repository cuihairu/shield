#define BOOST_TEST_MODULE DataPoolTests
#include <boost/test/unit_test.hpp>

#include "shield/config/config.hpp"
#include "shield/data/data.hpp"

#include <chrono>
#include <cstdint>

namespace {

void configure_mock_database(int64_t pool_size,
                             int64_t max_pool_size,
                             int64_t acquire_timeout_ms) {
    auto& cfg = shield::config::global_config();
    cfg.set("test_database.mock", true);
    cfg.set("test_database.pool_size", pool_size);
    cfg.set("test_database.max_pool_size", max_pool_size);
    cfg.set("test_database.acquire_timeout", acquire_timeout_ms);
}

void configure_mock_redis(int64_t pool_size,
                          int64_t max_pool_size,
                          int64_t acquire_timeout_ms) {
    auto& cfg = shield::config::global_config();
    cfg.set("test_redis.mock", true);
    cfg.set("test_redis.pool_size", pool_size);
    cfg.set("test_redis.max_pool_size", max_pool_size);
    cfg.set("test_redis.acquire_timeout", acquire_timeout_ms);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(DataPool)

BOOST_AUTO_TEST_CASE(DatabasePoolUsesConfiguredMockPoolAndExpandsToMax) {
    configure_mock_database(1, 2, 50);

    shield::data::DatabasePool pool;
    BOOST_REQUIRE(pool.initialize("test_database"));
    BOOST_REQUIRE(pool.is_initialized());

    auto first = pool.acquire();
    BOOST_REQUIRE(first);

    auto second = pool.acquire();
    BOOST_REQUIRE(second);

    first.reset();
    second.reset();

    auto result = pool.query("SELECT 1", {});
    BOOST_CHECK(result.success);
}

BOOST_AUTO_TEST_CASE(DatabasePoolAcquireTimesOutWhenExhausted) {
    configure_mock_database(1, 1, 25);

    shield::data::DatabasePool pool;
    BOOST_REQUIRE(pool.initialize("test_database"));

    auto held = pool.acquire();
    BOOST_REQUIRE(held);

    const auto start = std::chrono::steady_clock::now();
    auto blocked = pool.acquire();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    BOOST_CHECK(!blocked);
    BOOST_CHECK_EQUAL(pool.last_error_code(), "pool_exhausted");
    BOOST_CHECK_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                       .count(),
                   20);
}

BOOST_AUTO_TEST_CASE(DatabasePoolDoesNotFallbackToMockUnlessConfigured) {
    auto& cfg = shield::config::global_config();
    cfg.set("bad_database.mock", false);
    cfg.set("bad_database.allow_mock_fallback", false);
    cfg.set("bad_database.driver", std::string("unsupported"));
    cfg.set("bad_database.pool_size", int64_t{1});
    cfg.set("bad_database.max_pool_size", int64_t{1});

    shield::data::DatabasePool pool;
    BOOST_CHECK(!pool.initialize("bad_database"));
    BOOST_CHECK(!pool.is_initialized());
    // With the plugin-based loader, an unknown driver surfaces as
    // "module_unavailable" (no shield_db_<driver> DLL found), not the
    // legacy "unsupported_driver" code from compile-time #ifdef.
    BOOST_CHECK_EQUAL(pool.last_error_code(), "module_unavailable");
}

BOOST_AUTO_TEST_CASE(RedisPoolAcquireTimesOutWhenExhausted) {
    configure_mock_redis(1, 1, 25);

    shield::data::RedisPool pool;
    BOOST_REQUIRE(pool.initialize("test_redis"));

    auto held = pool.acquire();
    BOOST_REQUIRE(held);

    auto blocked = pool.acquire();
    BOOST_CHECK(!blocked);
    BOOST_CHECK_EQUAL(pool.last_error_code(), "pool_exhausted");
}

BOOST_AUTO_TEST_SUITE_END()
