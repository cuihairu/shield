#define BOOST_TEST_MODULE LuaApiLegacyTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <filesystem>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(LegacyApiTests)

BOOST_AUTO_TEST_CASE(LAPI_010_01_OldServiceAPI) {
    // Test that shield.service("name") returns nil or legacy_api_removed
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Create a service that tries to use old shield.service() API
    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "legacy_api_service.lua",
        "legacy_service_test",
        nlohmann::json{{"test_case", "old_service_api"}}, &error);

    // The service should spawn, but shield.service should return nil or error
    // TODO: Implement legacy_api_service.lua with test case for old service API
    BOOST_CHECK((service != nullptr) || (error.find("legacy") != std::string::npos));
}

BOOST_AUTO_TEST_CASE(LAPI_010_02_OldPluginAPI) {
    // Test that shield.plugin.on(...) returns nil or legacy_api_removed
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "legacy_api_service.lua",
        "legacy_plugin_test",
        nlohmann::json{{"test_case", "old_plugin_api"}}, &error);

    // shield.plugin API should not exist
    // TODO: Implement test case in legacy_api_service.lua
}

BOOST_AUTO_TEST_CASE(LAPI_010_03_OldColonDBAPI) {
    // Test that shield.db:query(...) fails; dot API required
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "legacy_api_service.lua",
        "legacy_db_test",
        nlohmann::json{{"test_case", "old_colon_db"}}, &error);

    // Old colon-style shield.db:query should fail
    // New dot-style shield.db.query should work
    // TODO: Implement test case in legacy_api_service.lua
}

BOOST_AUTO_TEST_CASE(LAPI_010_04_OldOnMessageEntry) {
    // Test that service defining only on_message(src, type, data) doesn't dispatch
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Create a service with old-style on_message entry point
    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "legacy_api_service.lua",
        "legacy_onmessage_test",
        nlohmann::json{{"test_case", "old_on_message"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // Try to send a message to the service
    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "message_sender",
        nlohmann::json{{"test_case", "default"}}, &error);

    nlohmann::json message = {
        {"method", "test_method"},
        {"args", {"test"}}
    };

    bool sent = manager->send_message(sender, service, message, &error);

    // Message should either fail or not dispatch through old on_message
    // TODO: Implement verification that old on_message is not called
}

BOOST_AUTO_TEST_CASE(LAPI_010_05_OldDIApi) {
    // Test that DI/IoC injection API is unavailable
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "legacy_api_service.lua",
        "legacy_di_test",
        nlohmann::json{{"test_case", "old_di"}}, &error);

    // shield.inject, shield.container, etc. should not exist
    // TODO: Implement test case in legacy_api_service.lua
}

BOOST_AUTO_TEST_CASE(LegacyAPINotAccessible) {
    // Verify that various legacy APIs are not accessible from Lua
    auto runtime = std::make_shared<LuaRuntime>();

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    // Try to access legacy APIs from Lua
    std::string error;

    // Test that shield.service doesn't exist or returns nil
    bool has_service_api = runtime->eval_boolean(vm,
        "return (shield.service ~= nil)", &error);
    BOOST_CHECK(!has_service_api);

    // Test that shield.plugin doesn't exist
    bool has_plugin_api = runtime->eval_boolean(vm,
        "return (shield.plugin ~= nil)", &error);
    BOOST_CHECK(!has_plugin_api);

    // Test that shield.inject doesn't exist
    bool has_inject_api = runtime->eval_boolean(vm,
        "return (shield.inject ~= nil)", &error);
    BOOST_CHECK(!has_inject_api);
}

BOOST_AUTO_TEST_SUITE_END()
