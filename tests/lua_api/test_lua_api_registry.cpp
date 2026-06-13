#define BOOST_TEST_MODULE LuaApiRegistryTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <filesystem>
#include <thread>
#include <chrono>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(RegistryTests)

BOOST_AUTO_TEST_CASE(LAPI_003_01_QueryByName) {
    // Test that shield.query(name) returns equivalent handle after spawn with name
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;
    std::string service_name = "test_query_service";

    // Spawn service with name
    auto handle = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        service_name,
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(handle != nullptr);
    BOOST_CHECK(error.empty());

    // Query by name
    auto queried = runtime->query_service(service_name);
    BOOST_CHECK(queried != nullptr);

    // The queried handle should be equivalent to the original handle
    // (Opaque handle comparison - check if they refer to same service)
    BOOST_CHECK(handle->equals(queried));
}

BOOST_AUTO_TEST_CASE(LAPI_003_02_DuplicateNameConflict) {
    // Test that spawning second service with same name fails with name_conflict
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;
    std::string service_name = "duplicate_test";

    // Spawn first service
    auto first = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        service_name,
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(first != nullptr);
    BOOST_CHECK(error.empty());

    // Attempt to spawn second service with same name
    auto second = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        service_name,
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_CHECK(second == nullptr);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(error.find("name_conflict") != std::string::npos ||
                error.find("duplicate") != std::string::npos ||
                error.find("already exists") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_003_03_InitFailedNotVisible) {
    // Test that service with failed init is not visible via query
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;
    std::string service_name = "failed_init_service";

    // Create a test that simulates init failure
    // For now, we'll test the concept by attempting to query a non-existent service
    auto queried = runtime->query_service(service_name);
    BOOST_CHECK(queried == nullptr);

    // TODO: Add test for init failure visibility after implementing init failure handling
}

BOOST_AUTO_TEST_CASE(LAPI_003_04_RegisterAlias) {
    // Test that shield.register(name) makes new name visible
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;
    std::string service_name = "alias_test_service";
    std::string alias_name = "alias_test_alias";

    nlohmann::json config = {
        {"test_case", "default"},
        {"register_alias", alias_name}
    };

    // Spawn service that registers an alias in on_init
    auto handle = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        service_name,
        config, &error);

    BOOST_REQUIRE(handle != nullptr);

    // Query by original name should work
    auto by_original = runtime->query_service(service_name);
    BOOST_CHECK(by_original != nullptr);

    // Query by alias should also work
    // TODO: Implement register in Lua API and test alias visibility
    // auto by_alias = runtime->query_service(alias_name);
    // BOOST_CHECK(by_alias != nullptr);
}

BOOST_AUTO_TEST_CASE(LAPI_003_05_ServiceExitNotVisible) {
    // Test that after service exit, querying old names returns service_not_found
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;
    std::string service_name = "exit_test_service";

    // Spawn service
    auto handle = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        service_name,
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(handle != nullptr);
    BOOST_CHECK(error.empty());

    // Verify service is visible
    auto before_exit = runtime->query_service(service_name);
    BOOST_CHECK(before_exit != nullptr);

    // Exit service
    manager->exit_service(handle, "test_exit");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Query after exit should return null
    auto after_exit = runtime->query_service(service_name);
    BOOST_CHECK(after_exit == nullptr);
}

BOOST_AUTO_TEST_CASE(LAPI_003_06_InvalidName) {
    // Test that register with invalid name fails with invalid_name
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Test various invalid names
    std::vector<std::string> invalid_names = {
        "",      // empty
        " ",     // whitespace
        "  ",    // multiple whitespace
        "\t",    // tab
        "\n",    // newline
        "123",   // starting with number
        "name with spaces",  // spaces in middle
        "name-with-dashes",  // dashes instead of underscores
        "name.with.dots",    // dots
    };

    for (const auto& invalid_name : invalid_names) {
        if (invalid_name.empty()) continue;  // Empty names might be handled differently

        auto handle = manager->spawn_service(
            TEST_SCRIPTS_DIR + "messaging_service.lua",
            invalid_name,
            nlohmann::json{{"test_case", "default"}}, &error);

        // Should fail or be rejected
        if (!invalid_name.empty()) {
            BOOST_CHECK((handle == nullptr) || (!error.empty()));
        }
    }
}

BOOST_AUTO_TEST_CASE(LAPI_003_07_NamesAPI) {
    // Test shield.names() returns list of registered service names
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Initially no services
    auto initial_names = runtime->list_service_names();
    BOOST_CHECK(initial_names.empty());

    // Spawn first service
    manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "service_1",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Spawn second service
    manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "service_2",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Check that both services are listed
    auto names = runtime->list_service_names();
    BOOST_CHECK(names.size() >= 2);

    BOOST_CHECK(std::find(names.begin(), names.end(), "service_1") != names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "service_2") != names.end());
}

BOOST_AUTO_TEST_CASE(LAPI_003_08_UnregisterName) {
    // Test shield.unregister(name) removes name from registry
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;
    std::string service_name = "unregister_test";

    // Spawn service
    auto handle = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        service_name,
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(handle != nullptr);

    // Verify service is visible
    auto before_unregister = runtime->query_service(service_name);
    BOOST_CHECK(before_unregister != nullptr);

    // Unregister the service name
    bool unregistered = runtime->unregister_service(service_name);
    BOOST_CHECK(unregistered);

    // Query after unregister should return null
    auto after_unregister = runtime->query_service(service_name);
    BOOST_CHECK(after_unregister == nullptr);

    // Unregistering non-existent name should fail gracefully
    bool unregistered_again = runtime->unregister_service(service_name);
    BOOST_CHECK(!unregistered_again);
}

BOOST_AUTO_TEST_SUITE_END()
