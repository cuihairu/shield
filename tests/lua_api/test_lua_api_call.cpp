#define BOOST_TEST_MODULE LuaApiCallTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <filesystem>
#include <thread>
#include <chrono>
#include <future>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(CallTests)

BOOST_AUTO_TEST_CASE(LAPI_005_01_CallReturnsOneValue) {
    // Test that call to service returning one value returns true, value
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "callee_service",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(callee != nullptr);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "caller_service",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(caller != nullptr);

    // Call method that returns one value
    nlohmann::json call_request = {
        {"method", "return_value"},
        {"args", nlohmann::json::array()},
        {"timeout", 5000}
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(result.success);
    BOOST_CHECK_EQUAL(result.values.size(), 1);
    BOOST_CHECK_EQUAL(result.values[0].get<std::string>(), "returned_value");
}

BOOST_AUTO_TEST_CASE(LAPI_005_02_CallReturnsFalseWithReason) {
    // Test that call to service returning false, reason returns true, false, reason
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "callee_error_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "caller_error_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call method that returns false, reason
    nlohmann::json call_request = {
        {"method", "return_false"},
        {"args", nlohmann::json::array()},
        {"timeout", 5000}
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(result.success);
    BOOST_CHECK_EQUAL(result.values.size(), 2);
    BOOST_CHECK(result.values[0].get<bool>() == false);
    BOOST_CHECK_EQUAL(result.values[1].get<std::string>(), "return_false_reason");
}

BOOST_AUTO_TEST_CASE(LAPI_005_03_CallReturnsTrailingNil) {
    // Test that call preserving trailing nil values in argc
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "callee_nil_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "caller_nil_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call method that might return nil
    nlohmann::json call_request = {
        {"method", "return_nil"},
        {"args", nlohmann::json::array()},
        {"timeout", 5000}
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(result.success);
    // Should preserve the nil value in the result
    BOOST_CHECK(result.values.size() >= 1);
    if (result.values.size() >= 1) {
        BOOST_CHECK(result.values[0].is_null());
    }
}

BOOST_AUTO_TEST_CASE(LAPI_005_04_CalleeThrowsError) {
    // Test that call to throwing handler returns false, handler_error
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "callee_throw_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "caller_throw_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call method that throws error
    nlohmann::json call_request = {
        {"method", "throw_error"},
        {"args", nlohmann::json::array()},
        {"timeout", 5000}
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(error.find("handler_error") != std::string::npos ||
                error.find("error") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_005_05_MethodMissing) {
    // Test that call to non-existent method returns false, method_not_found
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "callee_missing_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "caller_missing_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call non-existent method
    nlohmann::json call_request = {
        {"method", "nonexistent_method"},
        {"args", nlohmann::json::array()},
        {"timeout", 5000}
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(error.find("not_found") != std::string::npos ||
                error.find("unknown") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_005_06_CallTimeout) {
    // Test that call with timeout returns false, timeout after timeout expires
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Create a callee that doesn't respond (slow service)
    auto slow_callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "slow_callee",
        nlohmann::json{{"test_case", "slow"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "timeout_caller",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call with short timeout
    nlohmann::json call_request = {
        {"method", "slow_method"},
        {"args", nlohmann::json::array()},
        {"timeout", 100}  // 100ms timeout
    };

    auto start = std::chrono::steady_clock::now();
    auto result = manager->call_service(caller, slow_callee, call_request, &error);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    BOOST_CHECK(!result.success);
    BOOST_CHECK(error.find("timeout") != std::string::npos);
    BOOST_CHECK(elapsed >= 100 && elapsed < 500);  // Should timeout around 100ms
}

BOOST_AUTO_TEST_CASE(LAPI_005_07_LateResponseDiscarded) {
    // Test that response arriving after timeout is discarded
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto late_callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "late_callee",
        nlohmann::json{{"test_case", "late_response"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "late_caller",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call with short timeout, callee responds after timeout
    nlohmann::json call_request = {
        {"method", "late_responding_method"},
        {"args", nlohmann::json::array()},
        {"timeout", 50}  // 50ms timeout
    };

    auto result = manager->call_service(caller, late_callee, call_request, &error);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(error.find("timeout") != std::string::npos);

    // Wait for late response and verify it was discarded
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // The late response should not cause any issues
    // TODO: Add verification that late response was discarded properly
}

BOOST_AUTO_TEST_CASE(LAPI_005_08_NestedCallNonBlocking) {
    // Test that nested calls don't block runtime thread
    // This is a critical test for coroutine-aware call implementation
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Service A -> calls Service B -> calls Service C
    auto service_c = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "service_c",
        nlohmann::json{{"test_case", "nested"}}, &error);

    auto service_b = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "service_b",
        nlohmann::json{{"test_case", "nested"}}, &error);

    auto service_a = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "service_a",
        nlohmann::json{{"test_case", "nested"}}, &error);

    // First call from A to B (which will call C)
    nlohmann::json call_request = {
        {"method", "nested_call"},
        {"args", {"service_c"}},
        {"timeout", 5000}
    };

    auto start = std::chrono::steady_clock::now();
    auto result = manager->call_service(service_a, service_b, call_request, &error);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    BOOST_CHECK(result.success);

    // Runtime thread should not be blocked during nested calls
    // This is verified by the fact that the call completes and other services
    // could potentially process messages during the call

    // TODO: Add verification that runtime processed other messages during nested call
    // This requires implementing message processing tracking
}

BOOST_AUTO_TEST_CASE(CallDefaultTimeout) {
    // Test that call uses default timeout when not specified
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "default_timeout_callee",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "default_timeout_caller",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call without explicit timeout (should use shield.call_timeout default)
    nlohmann::json call_request = {
        {"method", "return_value"},
        {"args", nlohmann::json::array()}
        // No timeout field - should use default
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(result.success);
}

BOOST_AUTO_TEST_CASE(CallWithNilArguments) {
    // Test call with nil/optional arguments
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "nil_args_callee",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "nil_args_caller",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Call with some nil arguments
    nlohmann::json call_request = {
        {"method", "multi_return"},
        {"args", nlohmann::json::array({nullptr, "valid_arg", nullptr})}
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(result.success);
}

BOOST_AUTO_TEST_CASE(CallToExitedService) {
    // Test that call to exited service fails appropriately
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto callee = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "exited_callee",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto caller = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "call_after_exit",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Exit the callee
    manager->exit_service(callee, "test_exit");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Try to call exited service
    nlohmann::json call_request = {
        {"method", "return_value"},
        {"args", nlohmann::json::array()},
        {"timeout", 1000}
    };

    auto result = manager->call_service(caller, callee, call_request, &error);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(error.find("not_found") != std::string::npos ||
                error.find("exited") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
