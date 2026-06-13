#define BOOST_TEST_MODULE LuaApiDataTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <thread>
#include <chrono>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(DataApiTests)

BOOST_AUTO_TEST_CASE(LAPI_008_01_DbQueryWithFakePool) {
    // Test that shield.db.query works with fake/mock DB pool
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "data_service.lua",
        "db_query_test",
        nlohmann::json{{"test_case", "db_query"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: When DB API is fully implemented with mock pools:
    // 1. Configure fake DB pool
    // 2. Call shield.db.query
    // 3. Verify result: true, rows
    // For now, just verify service spawned
}

BOOST_AUTO_TEST_CASE(LAPI_008_02_DbQueryDisabled) {
    // Test that shield.db.query returns module_unavailable when DB disabled
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Service without DB enabled
    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "data_service.lua",
        "db_disabled_test",
        nlohmann::json{{"test_case", "db_disabled"}}, &error);

    // TODO: When DB API with enable/disable check is implemented:
    // 1. Configure without database.enabled
    // 2. Call shield.db.query
    // 3. Verify result: false, module_unavailable
}

BOOST_AUTO_TEST_CASE(LAPI_008_03_SqlError) {
    // Test that invalid SQL returns db_query_failed error
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "data_service.lua",
        "sql_error_test",
        nlohmann::json{{"test_case", "sql_error"}}, &error);

    // TODO: When DB API is fully implemented:
    // 1. Configure fake DB pool
    // 2. Call shield.db.query with invalid SQL
    // 3. Verify result: false, db_query_failed
}

BOOST_AUTO_TEST_CASE(LAPI_008_04_RedisGetWithFakePool) {
    // Test that shield.redis.get works with fake/mock Redis pool
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "data_service.lua",
        "redis_get_test",
        nlohmann::json{{"test_case", "redis_get"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: When Redis API is fully implemented with mock pools:
    // 1. Configure fake Redis pool
    // 2. Call shield.redis.get
    // 3. Verify result: true, value
}

BOOST_AUTO_TEST_CASE(LAPI_008_05_RedisGetDisabled) {
    // Test that shield.redis.get returns module_unavailable when Redis disabled
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "data_service.lua",
        "redis_disabled_test",
        nlohmann::json{{"test_case", "redis_disabled"}}, &error);

    // TODO: When Redis API with enable/disable check is implemented:
    // 1. Configure without redis.enabled
    // 2. Call shield.redis.get
    // 3. Verify result: false, module_unavailable
}

BOOST_AUTO_TEST_CASE(LAPI_008_06_SubscribeThenExit) {
    // Test that service exit cancels Redis subscriptions
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "data_service.lua",
        "subscribe_exit_test",
        nlohmann::json{{"test_case", "subscribe_exit"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: When Redis pub/sub is implemented:
    // 1. Subscribe to a channel
    // 2. Exit the service
    // 3. Verify subscription is cancelled
    // 4. Verify no memory leaks
}

BOOST_AUTO_TEST_CASE(DbAPIExists) {
    // Test that DB API functions exist
    auto runtime = std::make_shared<LuaRuntime>();

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    // TODO: When DB API is implemented, verify:
    // - shield.db exists
    // - shield.db.query exists
    // - shield.db.execute exists
    // - shield.db.prepare exists (if prepared statements are supported)
}

BOOST_AUTO_TEST_CASE(RedisAPIExists) {
    // Test that Redis API functions exist
    auto runtime = std::make_shared<LuaRuntime>();

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    // TODO: When Redis API is implemented, verify:
    // - shield.redis exists
    // - shield.redis.get exists
    // - shield.redis.set exists
    // - shield.redis.del exists
    // - shield.redis.subscribe exists
    // - shield.redis.unsubscribe exists
}

BOOST_AUTO_TEST_CASE(DbAPIDotNotationRequired) {
    // Test that old colon notation (db:query) doesn't work
    auto runtime = std::make_shared<LuaRuntime>();

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    std::string error;

    // TODO: When DB API is implemented, verify that
    // old colon-style notation fails or is not available
}

BOOST_AUTO_TEST_SUITE_END()
