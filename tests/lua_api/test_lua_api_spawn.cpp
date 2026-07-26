#define BOOST_TEST_MODULE LuaApiSpawnTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json opts_for(const std::string& name,
                        nlohmann::json args = nlohmann::json::object()) {
    return {
        {"name", name},
        {"args", std::move(args)},
        {"config", nlohmann::json::object()},
    };
}

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

// Poll a recorder method until it returns a non-nil value.
CallResult call_once(LuaServiceManager& manager, const std::string& service,
                     const std::string& method) {
    return manager.call(service, method, nlohmann::json::array(), 1000);
}
}  // namespace

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(SpawnTests)

// shield.spawn on a VM main thread (on_init) keeps its synchronous behavior
// and returns a usable ServiceHandle.
BOOST_AUTO_TEST_CASE(SpawnFromOnInitUsesSyncPath) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    nlohmann::json args = {
        {"child_script", TEST_SCRIPTS_DIR + "spawn_child.lua"},
        {"child_name", "init_child"}};
    auto parent = manager.spawn(TEST_SCRIPTS_DIR + "spawn_in_init_parent.lua",
                                opts_for("init_parent", args).dump());
    BOOST_REQUIRE(parent.success);

    CallResult got = call_once(manager, parent.service_id, "get_child");
    BOOST_REQUIRE(got.success);
    BOOST_REQUIRE_EQUAL(got.values.size(), 2u);
    BOOST_CHECK_EQUAL(got.values[0].get<std::string>(), "init_child");
    BOOST_CHECK_EQUAL(got.values[1].get<uint32_t>(), 0u);

    // The child is published and answers calls.
    BOOST_CHECK_EQUAL(manager.query_service("init_child"), "init_child");
    CallResult pong = call_once(manager, "init_child", "ping");
    BOOST_REQUIRE(pong.success);
    BOOST_REQUIRE_EQUAL(pong.values.size(), 1u);
    BOOST_CHECK_EQUAL(pong.values[0].get<std::string>(), "pong:init_child");
}

// shield.spawn inside a handler coroutine suspends instead of blocking the
// caller's service actor: the parent keeps answering messages while the
// child's slow on_init runs on the spawn worker.
BOOST_AUTO_TEST_CASE(CoroutineSpawnDoesNotBlockCallerActor) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto parent = manager.spawn(TEST_SCRIPTS_DIR + "spawn_parent.lua",
                                opts_for("co_parent").dump());
    BOOST_REQUIRE(parent.success);

    BOOST_REQUIRE(
        manager.send(parent.service_id, "spawn_and_record",
                     nlohmann::json::array(
                         {TEST_SCRIPTS_DIR + "spawn_child.lua", "co_child",
                          nlohmann::json::object({{"init_sleep_ms", 500}})})));

    // While the child's 500ms init is in flight the parent must still answer.
    const auto start = std::chrono::steady_clock::now();
    CallResult pong =
        manager.call(parent.service_id, "ping", nlohmann::json::array(), 2000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    BOOST_REQUIRE(pong.success);
    BOOST_CHECK_LT(elapsed.count(), 400);

    // The suspended spawn completes and records a valid handle.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult ok =
                call_once(manager, parent.service_id, "get_last_ok");
            return ok.success && ok.values.size() == 1u &&
                   ok.values[0].is_boolean() && ok.values[0].get<bool>();
        },
        std::chrono::seconds(3)));

    CallResult id = call_once(manager, parent.service_id, "get_last_id");
    BOOST_REQUIRE(id.success);
    BOOST_REQUIRE_EQUAL(id.values.size(), 1u);
    BOOST_CHECK_EQUAL(id.values[0].get<std::string>(), "co_child");

    CallResult node = call_once(manager, parent.service_id, "get_last_node");
    BOOST_REQUIRE(node.success);
    BOOST_REQUIRE_EQUAL(node.values.size(), 1u);
    BOOST_CHECK_EQUAL(node.values[0].get<uint32_t>(), 0u);

    BOOST_CHECK_EQUAL(manager.query_service("co_child"), "co_child");
}

// While an async spawn holds its name reservation, a same-name spawn attempt
// fails fast instead of waiting or stealing the name.
BOOST_AUTO_TEST_CASE(ReservedNameRejectsConcurrentDuplicate) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto parent = manager.spawn(TEST_SCRIPTS_DIR + "spawn_parent.lua",
                                opts_for("rsv_parent").dump());
    BOOST_REQUIRE(parent.success);

    BOOST_REQUIRE(manager.send(
        parent.service_id, "spawn_and_record",
        nlohmann::json::array(
            {TEST_SCRIPTS_DIR + "spawn_child.lua", "reserved_child",
             nlohmann::json::object({{"init_sleep_ms", 500}})})));

    // Give the spawn worker a moment to pick up the job and reserve the name
    // (the reservation is taken before VM creation; init then runs 500ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SpawnResult dup = manager.spawn(TEST_SCRIPTS_DIR + "spawn_child.lua",
                                    opts_for("reserved_child").dump());
    BOOST_CHECK(!dup.success);
    BOOST_CHECK(dup.error_message.find("already reserved") !=
                std::string::npos);

    // The original async spawn still completes normally.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult ok =
                call_once(manager, parent.service_id, "get_last_ok");
            return ok.success && ok.values.size() == 1u &&
                   ok.values[0].is_boolean() && ok.values[0].get<bool>();
        },
        std::chrono::seconds(3)));
    BOOST_CHECK_EQUAL(manager.query_service("reserved_child"),
                      "reserved_child");
}

// A failed on_init reports init_failed to the suspended caller and rolls the
// name back so it can be spawned again.
BOOST_AUTO_TEST_CASE(FailedInitRollsBackName) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto parent = manager.spawn(TEST_SCRIPTS_DIR + "spawn_parent.lua",
                                opts_for("fail_parent").dump());
    BOOST_REQUIRE(parent.success);

    BOOST_REQUIRE(manager.send(
        parent.service_id, "spawn_and_record",
        nlohmann::json::array(
            {TEST_SCRIPTS_DIR + "spawn_fail_child.lua", "flaky_child"})));

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult ok =
                call_once(manager, parent.service_id, "get_last_ok");
            return ok.success && ok.values.size() == 1u &&
                   ok.values[0].is_boolean() && !ok.values[0].get<bool>();
        },
        std::chrono::seconds(3)));

    CallResult err = call_once(manager, parent.service_id, "get_last_err");
    BOOST_REQUIRE(err.success);
    BOOST_REQUIRE_EQUAL(err.values.size(), 1u);
    BOOST_REQUIRE(err.values[0].is_object());
    BOOST_CHECK_EQUAL(err.values[0].value("code", ""), "init_failed");

    // Name was rolled back: spawning a healthy child under it now works.
    BOOST_REQUIRE(manager.send(
        parent.service_id, "spawn_and_record",
        nlohmann::json::array(
            {TEST_SCRIPTS_DIR + "spawn_child.lua", "flaky_child"})));

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult ok =
                call_once(manager, parent.service_id, "get_last_ok");
            return ok.success && ok.values.size() == 1u &&
                   ok.values[0].is_boolean() && ok.values[0].get<bool>();
        },
        std::chrono::seconds(3)));
    BOOST_CHECK_EQUAL(manager.query_service("flaky_child"), "flaky_child");
}

// shield.exit from a Lua handler stops the service and unpublishes its name.
BOOST_AUTO_TEST_CASE(ExitFromLuaHandler) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto parent = manager.spawn(TEST_SCRIPTS_DIR + "spawn_parent.lua",
                                opts_for("exit_parent").dump());
    BOOST_REQUIRE(parent.success);
    BOOST_REQUIRE_EQUAL(manager.query_service("exit_parent"), "exit_parent");

    BOOST_REQUIRE(manager.send(parent.service_id, "exit_now",
                               nlohmann::json::array({"normal"})));

    BOOST_CHECK(wait_until(
        [&]() { return manager.query_service("exit_parent").empty(); },
        std::chrono::seconds(3)));
}

// ServiceHandle:node() is 0 (local) in the single-node runtime.
BOOST_AUTO_TEST_CASE(SelfHandleNodeIsLocal) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto parent = manager.spawn(TEST_SCRIPTS_DIR + "spawn_parent.lua",
                                opts_for("node_parent").dump());
    BOOST_REQUIRE(parent.success);

    CallResult node = call_once(manager, parent.service_id, "self_node");
    BOOST_REQUIRE(node.success);
    BOOST_REQUIRE_EQUAL(node.values.size(), 1u);
    BOOST_CHECK_EQUAL(node.values[0].get<uint32_t>(), 0u);
}

// shield.panic invokes on_panic(reason, {type="explicit"}) and then exits
// the service.
BOOST_AUTO_TEST_CASE(PanicFromLuaInvokesHookAndExits) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto watcher = manager.spawn(TEST_SCRIPTS_DIR + "spawn_watcher.lua",
                                 opts_for("panic_watcher").dump());
    BOOST_REQUIRE(watcher.success);

    auto panicky = manager.spawn(
        TEST_SCRIPTS_DIR + "spawn_panic_service.lua",
        opts_for("panicky", {{"watcher", "panic_watcher"}}).dump());
    BOOST_REQUIRE(panicky.success);

    BOOST_REQUIRE(
        manager.send(panicky.service_id, "boom", nlohmann::json::array()));

    // The panic report reaches the watcher with the explicit context.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult events =
                call_once(manager, watcher.service_id, "get_events");
            return events.success && events.values.size() == 1u &&
                   events.values[0].is_array() && events.values[0].size() == 1u;
        },
        std::chrono::seconds(3)));

    CallResult events = call_once(manager, watcher.service_id, "get_events");
    BOOST_REQUIRE(events.success);
    const auto& first = events.values[0][0];
    BOOST_CHECK_EQUAL(first.value("reason", ""), "test panic");
    BOOST_CHECK_EQUAL(first.value("type", ""), "explicit");

    // The service exits with reason "panic".
    BOOST_CHECK(
        wait_until([&]() { return manager.query_service("panicky").empty(); },
                   std::chrono::seconds(3)));
}

BOOST_AUTO_TEST_SUITE_END()
