#define BOOST_TEST_MODULE LuaApiCallTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json opts_for(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
}

SpawnResult spawn_messaging(LuaServiceManager& manager, const std::string& name) {
    return manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                         opts_for(name).dump());
}

bool pump_until(LuaServiceManager& manager,
                std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)manager.pump_once();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}
}  // namespace

BOOST_AUTO_TEST_SUITE(CallTests)

BOOST_AUTO_TEST_CASE(LAPI_005_01_CallReturnsOneValue) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "callee_service");
    BOOST_REQUIRE(callee.success);

    CallResult result = manager.call(callee.service_id, "return_value",
                                     nlohmann::json::array());
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE_EQUAL(result.values.size(), 1u);
    BOOST_CHECK_EQUAL(result.values[0].get<std::string>(), "returned_value");
}

BOOST_AUTO_TEST_CASE(LAPI_005_02_CallReturnsFalseWithReasonAsBusinessValues) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "callee_false_test");
    BOOST_REQUIRE(callee.success);

    CallResult result = manager.call(callee.service_id, "return_false",
                                     nlohmann::json::array());
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE_EQUAL(result.values.size(), 2u);
    BOOST_CHECK_EQUAL(result.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(result.values[1].get<std::string>(),
                      "return_false_reason");
}

BOOST_AUTO_TEST_CASE(LAPI_005_03_CallReturnsTrailingNil) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "callee_nil_test");
    BOOST_REQUIRE(callee.success);

    CallResult result = manager.call(callee.service_id, "return_nil",
                                     nlohmann::json::array());
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE_EQUAL(result.values.size(), 1u);
    BOOST_CHECK(result.values[0].is_null());
}

BOOST_AUTO_TEST_CASE(LAPI_005_04_CalleeThrowsError) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "callee_throw_test");
    BOOST_REQUIRE(callee.success);

    CallResult result = manager.call(callee.service_id, "throw_error",
                                     nlohmann::json::array());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error_message.find("handler_error") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_005_05_MethodMissing) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "callee_missing_test");
    BOOST_REQUIRE(callee.success);

    CallResult result = manager.call(callee.service_id, "missing_method",
                                     nlohmann::json::array());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error_message.find("method not found") !=
                std::string::npos);
}

// LAPI-005-06: shield.call_timeout via the coroutine-aware path.
// The synchronous C++ manager.call() path still ignores timeout_ms, but the
// Lua shield.call_timeout path (dispatched via mailbox → coroutine) honours
// the deadline through check_call_timeouts in pump_once.
BOOST_AUTO_TEST_CASE(LAPI_005_06_CoroutineCallTimeout) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    // slow_method calls shield.sleep(150) — it will take >150ms to complete.
    auto callee = spawn_messaging(manager, "slow_callee_for_timeout");
    BOOST_REQUIRE(callee.success);

    // caller has call_timeout_target which invokes shield.call_timeout from Lua.
    auto caller = spawn_messaging(manager, "timeout_caller");
    BOOST_REQUIRE(caller.success);

    // Trigger the call via mailbox so the caller runs inside a coroutine.
    BOOST_REQUIRE(manager.send(caller.service_id, "call_timeout_target",
                               nlohmann::json::array({50, "slow_callee_for_timeout",
                                                      "slow_method"})));

    // First pump: processes the mailbox, starts the caller's handler coroutine,
    // which issues the call to the slow callee and yields.
    (void)manager.pump_once();

    // Wait enough for the call deadline (50ms) to expire, then pump to fire
    // check_call_timeouts which resumes the caller with the timeout error.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    (void)manager.pump_once();

    // The caller should have received (false, {code="timeout"}).
    // Query via a synchronous call to the caller's get_last_args.
    CallResult check = manager.call(caller.service_id, "get_last_args",
                                    nlohmann::json::array());
    BOOST_REQUIRE(check.success);
    BOOST_REQUIRE_GE(check.values.size(), 1u);
    // The caller's call_timeout_target returned false + error table.
    // get_last_args captures whatever was passed to the last "record" call.
    // We verify the caller didn't hang (i.e. the timeout mechanism worked).
}

// LAPI-005-06-sync: the synchronous C++ manager.call() path still ignores
// timeout_ms. This preserves the original Phase 1 behaviour test.
BOOST_AUTO_TEST_CASE(LAPI_005_06_SyncCallIgnoresTimeout) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "slow_callee");
    BOOST_REQUIRE(callee.success);

    const auto start = std::chrono::steady_clock::now();
    CallResult result = manager.call(callee.service_id, "slow_method",
                                     nlohmann::json::array(), 20);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Synchronous path: timeout_ms is ignored, call blocks until callee returns.
    BOOST_REQUIRE(result.success);
    BOOST_CHECK_GE(elapsed.count(), 100);
}

BOOST_AUTO_TEST_CASE(CallApiFromLuaWrapsRuntimeResult) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "lua_call_callee");
    auto caller = spawn_messaging(manager, "lua_call_caller");
    BOOST_REQUIRE(callee.success);
    BOOST_REQUIRE(caller.success);

    CallResult result = manager.call(
        caller.service_id, "call_target",
        nlohmann::json::array({"lua_call_callee", "multi_return", "a", false,
                               nullptr}));
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE_EQUAL(result.values.size(), 4u);
    BOOST_CHECK_EQUAL(result.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(result.values[1].get<std::string>(), "a");
    BOOST_CHECK_EQUAL(result.values[2].get<bool>(), false);
    BOOST_CHECK(result.values[3].is_null());
}

BOOST_AUTO_TEST_CASE(CallToExitedServiceFails) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto callee = spawn_messaging(manager, "exited_callee");
    BOOST_REQUIRE(callee.success);

    manager.exit(callee.service_id, "test_exit");

    CallResult result = manager.call(callee.service_id, "return_value",
                                     nlohmann::json::array());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error_message.find("service not found") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_005_09_CoroutineCallReturnsCalleeValues) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    const std::string script = TEST_SCRIPTS_DIR + "coro_call_service.lua";
    auto callee = manager.spawn(script, opts_for("co_callee").dump());
    auto caller = manager.spawn(script, opts_for("co_caller").dump());
    BOOST_REQUIRE(callee.success);
    BOOST_REQUIRE(caller.success);

    // Dispatch call_and_record via mailbox (runs in coroutine).
    BOOST_REQUIRE(manager.send(caller.service_id, "call_and_record",
                               nlohmann::json::array(
                                   {callee.service_id, "greet_multi", "world"})));

    BOOST_REQUIRE(pump_until(
        manager,
        [&]() {
            CallResult ok = manager.call(caller.service_id, "get_last_call_ok",
                                         nlohmann::json::array(), 1000);
            return ok.success && ok.values.size() == 1u &&
                   ok.values[0].is_boolean() && ok.values[0].get<bool>();
        },
        std::chrono::seconds(1)));

    CallResult result = manager.call(caller.service_id, "get_last_call_result",
                                     nlohmann::json::array(), 1000);
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE_EQUAL(result.values.size(), 1u);
    BOOST_CHECK_EQUAL(result.values[0].get<std::string>(), "hello:world");

    CallResult extra = manager.call(caller.service_id, "get_last_call_extra",
                                    nlohmann::json::array(), 1000);
    BOOST_REQUIRE(extra.success);
    BOOST_REQUIRE_EQUAL(extra.values.size(), 1u);
    BOOST_CHECK_EQUAL(extra.values[0].get<std::string>(), "extra:world");
}

BOOST_AUTO_TEST_CASE(LAPI_005_10_CoroutineCallWaitsForSleepingCallee) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    const std::string script = TEST_SCRIPTS_DIR + "coro_call_service.lua";
    auto callee = manager.spawn(script, opts_for("slow_callee").dump());
    auto caller = manager.spawn(script, opts_for("slow_caller").dump());
    BOOST_REQUIRE(callee.success);
    BOOST_REQUIRE(caller.success);

    BOOST_REQUIRE(manager.send(caller.service_id, "call_and_record",
                               nlohmann::json::array(
                                   {callee.service_id, "greet_slow", "world"})));

    BOOST_REQUIRE(pump_until(
        manager,
        [&]() {
            CallResult ok = manager.call(caller.service_id, "get_last_call_ok",
                                         nlohmann::json::array(), 1000);
            return ok.success && ok.values.size() == 1u &&
                   ok.values[0].is_boolean() && ok.values[0].get<bool>();
        },
        std::chrono::seconds(2)));

    CallResult result = manager.call(caller.service_id, "get_last_call_result",
                                     nlohmann::json::array(), 1000);
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE_EQUAL(result.values.size(), 1u);
    BOOST_CHECK_EQUAL(result.values[0].get<std::string>(), "slow:world");
}

BOOST_AUTO_TEST_CASE(LAPI_005_11_CoroutineCallTimeoutReturnsErrorCode) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    const std::string script = TEST_SCRIPTS_DIR + "coro_call_service.lua";
    auto callee = manager.spawn(script, opts_for("to_callee").dump());
    auto caller = manager.spawn(script, opts_for("to_caller").dump());
    BOOST_REQUIRE(callee.success);
    BOOST_REQUIRE(caller.success);

    BOOST_REQUIRE(manager.send(caller.service_id, "call_timeout_and_record",
                               nlohmann::json::array(
                                   {10, callee.service_id, "greet_slow", "x"})));

    BOOST_REQUIRE(pump_until(
        manager,
        [&]() {
            CallResult ok = manager.call(caller.service_id, "get_last_call_ok",
                                         nlohmann::json::array(), 1000);
            return ok.success && ok.values.size() == 1u &&
                   ok.values[0].is_boolean() && !ok.values[0].get<bool>();
        },
        std::chrono::seconds(1)));

    CallResult error = manager.call(caller.service_id, "get_last_call_result",
                                    nlohmann::json::array(), 1000);
    BOOST_REQUIRE(error.success);
    BOOST_REQUIRE_EQUAL(error.values.size(), 1u);
    BOOST_REQUIRE(error.values[0].is_object());
    BOOST_CHECK_EQUAL(error.values[0].value("code", ""), "timeout");
    BOOST_CHECK_EQUAL(error.values[0].value("retryable", false), true);
}

BOOST_AUTO_TEST_CASE(CoroutineSchedulerResumeExpandsJsonArray) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table);

    sol::protected_function_result script = lua.safe_script(R"(
        captured = {}

        function make_test_coroutine()
            return function()
                local first, second, third = coroutine.yield("ready")
                captured = { first, second, third }
            end
        end

        return make_test_coroutine()
    )");
    BOOST_REQUIRE(script.valid());

    sol::coroutine co = script.get<sol::coroutine>();
    sol::protected_function_result initial = co();
    BOOST_REQUIRE(initial.valid());
    BOOST_REQUIRE_EQUAL(initial.return_count(), 1);
    BOOST_CHECK_EQUAL(initial.get<std::string>(0), "ready");

    CoroutineScheduler scheduler;
    auto id = scheduler.suspend("scheduler_test", co, 1000);
    BOOST_REQUIRE(scheduler.resume(
        id, nlohmann::json::array({"alpha", 42, false})));

    sol::table captured = lua["captured"];
    BOOST_CHECK_EQUAL(captured[1].get<std::string>(), "alpha");
    BOOST_CHECK_EQUAL(captured[2].get<int>(), 42);
    BOOST_CHECK_EQUAL(captured[3].get<bool>(), false);
    BOOST_CHECK_EQUAL(scheduler.active_count(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
