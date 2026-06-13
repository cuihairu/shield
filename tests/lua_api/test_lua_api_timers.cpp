#define BOOST_TEST_MODULE LuaApiTimersTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <thread>
#include <chrono>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(TimersAndTasksTests)

BOOST_AUTO_TEST_CASE(LAPI_007_01_TimerOnce) {
    // Test that timer_once callback is invoked once after delay
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "timer_service.lua",
        "timer_once_test",
        nlohmann::json{{"test_case", "timer_once"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: Implement shield.timer_once in Lua API and test it
    // For now, just verify service spawned
    BOOST_CHECK(service != nullptr);

    // Test should:
    // 1. Register a timer with timer_once
    // 2. Advance clock or wait for timeout
    // 3. Verify callback was called exactly once
}

BOOST_AUTO_TEST_CASE(LAPI_007_02_TimerFixedDelay) {
    // Test that fixed-delay timer runs callback after previous completion
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "timer_service.lua",
        "timer_fixed_test",
        nlohmann::json{{"test_case", "fixed_delay"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: Implement shield.timer in Lua API and test it
    // Test should:
    // 1. Register a repeating timer
    // 2. Advance clock through multiple periods
    // 3. Verify each callback completes before next starts
}

BOOST_AUTO_TEST_CASE(LAPI_007_03_CancelTimer) {
    // Test that canceled timer doesn't fire
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "timer_service.lua",
        "timer_cancel_test",
        nlohmann::json{{"test_case", "cancel"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: Implement timer:cancel() and test it
    // Test should:
    // 1. Register a timer
    // 2. Cancel the timer before it fires
    // 3. Advance clock past timeout
    // 4. Verify callback was never called
}

BOOST_AUTO_TEST_CASE(LAPI_007_04_TimerError) {
    // Test that timer callback error is handled and timer stops
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "timer_service.lua",
        "timer_error_test",
        nlohmann::json{{"test_case", "error"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: Implement timer error handling and test it
    // Test should:
    // 1. Register a timer with error-throwing callback
    // 2. Advance clock to trigger timer
    // 3. Verify error is logged
    // 4. Verify timer is stopped and doesn't re-fire
}

BOOST_AUTO_TEST_CASE(LAPI_007_05_SleepNonBlocking) {
    // Test that shield.sleep doesn't block runtime thread
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "timer_service.lua",
        "sleep_test",
        nlohmann::json{{"test_case", "sleep"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: Implement shield.sleep and test coroutine behavior
    // Test should:
    // 1. Call a function that uses shield.sleep
    // 2. Verify runtime can process other messages during sleep
    // 3. Verify function resumes after sleep duration
}

BOOST_AUTO_TEST_CASE(LAPI_007_06_ForkTask) {
    // Test that shield.fork creates independent task
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "timer_service.lua",
        "fork_test",
        nlohmann::json{{"test_case", "fork"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: Implement shield.fork and test it
    // Test should:
    // 1. Spawn a task with shield.fork
    // 2. Verify task runs independently
    // 3. Verify owner service tracks the task
    // 4. Verify task can be cancelled
}

BOOST_AUTO_TEST_CASE(LAPI_007_07_ServiceExitCancelsTimers) {
    // Test that service exit cancels owned timers and tasks
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "timer_service.lua",
        "exit_cancels_test",
        nlohmann::json{{"test_case", "exit"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // TODO: Implement automatic cleanup on exit
    // Test should:
    // 1. Spawn service with active timers/tasks
    // 2. Exit the service
    // 3. Verify timers/tasks are cancelled
    // 4. Verify no callbacks fire after exit
}

BOOST_AUTO_TEST_CASE(TimerAPIBasics) {
    // Basic test that timer API exists and has correct structure
    auto runtime = std::make_shared<LuaRuntime>();

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    // TODO: When timer API is implemented, test:
    // - shield.timer_once exists
    // - shield.timer exists
    // - shield.sleep exists
    // - shield.fork exists
    // - Timer handle has :cancel() method
}

BOOST_AUTO_TEST_CASE(NowAPI) {
    // Test shield.now() returns monotonic timestamp
    auto runtime = std::make_shared<LuaRuntime>();

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    std::string error;
    int64_t now1 = runtime->eval_int64(vm, "return shield.now()", &error);
    BOOST_CHECK(now1 > 0);

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int64_t now2 = runtime->eval_int64(vm, "return shield.now()", &error);
    BOOST_CHECK(now2 > now1);
}

BOOST_AUTO_TEST_SUITE_END()
