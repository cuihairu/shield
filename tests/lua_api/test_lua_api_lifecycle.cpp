#define BOOST_TEST_MODULE LuaApiLifecycleTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <filesystem>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(LifecycleTests)

BOOST_AUTO_TEST_CASE(LAPI_001_01_ValidModuleTable) {
    // Test that a service can be spawned from a valid module that returns a table
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    // Create a simple service VM
    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    std::string error;
    bool loaded = runtime->load_service_module(vm,
        TEST_SCRIPTS_DIR + "lifecycle_service.lua", &error);

    BOOST_REQUIRE(loaded);
    BOOST_CHECK(error.empty());
}

BOOST_AUTO_TEST_CASE(LAPI_002_01_OnInitNoReturnValue) {
    // Test that on_init with no return value succeeds
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    std::string error;
    bool loaded = runtime->load_service_module(vm,
        TEST_SCRIPTS_DIR + "lifecycle_service.lua", &error);

    BOOST_REQUIRE(loaded);

    // Call on_init with default config
    nlohmann::json args = R"({
        "name": "test_service",
        "config": {"test_case": "default"}
    })"_json;

    bool init_result = runtime->call_service_function(vm, "on_init", args, &error);
    BOOST_CHECK(init_result);
    BOOST_CHECK(error.empty());
}

BOOST_AUTO_TEST_CASE(LAPI_002_02_OnInitReturnsTrue) {
    // Test that on_init returning true succeeds
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    std::string error;
    bool loaded = runtime->load_service_module(vm,
        TEST_SCRIPTS_DIR + "lifecycle_service.lua", &error);

    BOOST_REQUIRE(loaded);

    nlohmann::json args = R"({
        "name": "test_service",
        "config": {"test_case": "return_true"}
    })"_json;

    bool init_result = runtime->call_service_function(vm, "on_init", args, &error);
    BOOST_CHECK(init_result);
}

BOOST_AUTO_TEST_CASE(LAPI_002_03_OnInitReturnsFalse) {
    // Test that on_init returning false is handled
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    std::string error;
    bool loaded = runtime->load_service_module(vm,
        TEST_SCRIPTS_DIR + "lifecycle_service.lua", &error);

    BOOST_REQUIRE(loaded);

    nlohmann::json args = R"({
        "name": "test_service",
        "config": {"test_case": "return_false"}
    })"_json;

    bool init_result = runtime->call_service_function(vm, "on_init", args, &error);
    // on_init returning false should still succeed the call, but the return value indicates failure
    BOOST_CHECK(init_result);
}

BOOST_AUTO_TEST_CASE(LAPI_002_04_OnInitThrowsError) {
    // Test that on_init throwing an error is captured
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    std::string error;
    bool loaded = runtime->load_service_module(vm,
        TEST_SCRIPTS_DIR + "lifecycle_service.lua", &error);

    BOOST_REQUIRE(loaded);

    nlohmann::json args = R"({
        "name": "test_service",
        "config": {"test_case": "throw_error"}
    })"_json;

    bool init_result = runtime->call_service_function(vm, "on_init", args, &error);
    BOOST_CHECK(!init_result);
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_SUITE_END()
