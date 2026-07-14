#define BOOST_TEST_MODULE LuaApiCafBridgeTests
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

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

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

nlohmann::json timer_opts_for(const std::string& name,
                              const std::string& test_case) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", {{"test_case", test_case}}},
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
}  // namespace

BOOST_AUTO_TEST_SUITE(CafBridgeTests)

BOOST_AUTO_TEST_CASE(NoActorSystemMeansNoServiceActor) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                                opts_for("no_caf").dump());
    BOOST_REQUIRE_MESSAGE(result.success,
                          "spawn failed: " + result.error_message);
    BOOST_CHECK(!manager.has_service_actor("no_caf"));
}

BOOST_AUTO_TEST_CASE(SpawnCreatesCafActorHandleInternally) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto result = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                                opts_for("caf_spawned").dump());
    BOOST_REQUIRE(result.success);
    BOOST_CHECK(manager.has_service_actor("caf_spawned"));
}

BOOST_AUTO_TEST_CASE(ExitRemovesCafActorMapping) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto result = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                                opts_for("caf_exit_test").dump());
    BOOST_REQUIRE(result.success);
    BOOST_CHECK(manager.has_service_actor("caf_exit_test"));

    manager.exit(result.service_id, "test_exit");
    BOOST_CHECK(!manager.has_service_actor("caf_exit_test"));
}

BOOST_AUTO_TEST_CASE(CafActorMappingCleanedOnShutdownAll) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto a = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                           opts_for("caf_shutdown_a").dump());
    auto b = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                           opts_for("caf_shutdown_b").dump());
    BOOST_REQUIRE(a.success);
    BOOST_REQUIRE(b.success);
    BOOST_CHECK(manager.has_service_actor("caf_shutdown_a"));
    BOOST_CHECK(manager.has_service_actor("caf_shutdown_b"));

    manager.shutdown_all("shutdown_test");
    BOOST_CHECK(!manager.has_service_actor("caf_shutdown_a"));
    BOOST_CHECK(!manager.has_service_actor("caf_shutdown_b"));
}

BOOST_AUTO_TEST_CASE(CafActorMappingCleanedOnDestruction) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    {
        LuaServiceManager manager(runtime);
        manager.attach_actor_system(system);

        auto result = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                                    opts_for("caf_destructor").dump());
        BOOST_REQUIRE(result.success);
        BOOST_CHECK(manager.has_service_actor("caf_destructor"));
        // manager goes out of scope; no crash expected
    }
    // If we reach here, destruction was clean
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(LuaApiUnchangedWithCafSystem) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    // send still works without requiring the legacy worker thread
    auto s = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                           opts_for("caf_api_echo").dump());
    BOOST_REQUIRE(s.success);
    BOOST_CHECK(
        manager.send("caf_api_echo", "echo", nlohmann::json::array({"hello"})));

    bool observed_send = false;
    for (int i = 0; i < 20; ++i) {
        CallResult seen = manager.call(s.service_id, "get_last_method",
                                       nlohmann::json::array());
        BOOST_REQUIRE(seen.success);
        if (seen.values.size() == 1u && seen.values[0].is_string() &&
            seen.values[0].get<std::string>() == "echo") {
            observed_send = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_CHECK(observed_send);

    // call still works
    CallResult cr =
        manager.call(s.service_id, "echo", nlohmann::json::array({"world"}));
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), "world");
}

BOOST_AUTO_TEST_CASE(ForkExecutesWithoutWorkerWhenActorSystemAttached) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto result = manager.spawn(TEST_SCRIPTS_DIR + "timer_service.lua",
                                timer_opts_for("caf_fork", "fork").dump());
    BOOST_REQUIRE(result.success);

    // Fork stays on the pump path (drained by pump_once), so drive it here.
    BOOST_CHECK(wait_until(
        [&]() {
            (void)manager.pump_once();
            CallResult cr = manager.call(result.service_id, "get_fork_count",
                                         nlohmann::json::array());
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].get<int>() >= 1;
        },
        std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(TimerOnceFiresWithoutWorkerWhenActorSystemAttached) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto result =
        manager.spawn(TEST_SCRIPTS_DIR + "timer_service.lua",
                      timer_opts_for("caf_timer_once", "timer_once").dump());
    BOOST_REQUIRE(result.success);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(result.service_id, "get_timer_count",
                                         nlohmann::json::array());
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].get<int>() >= 1;
        },
        std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(RepeatingTimerReschedulesViaCaf) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto result =
        manager.spawn(TEST_SCRIPTS_DIR + "timer_service.lua",
                      timer_opts_for("caf_timer_repeat", "fixed_delay").dump());
    BOOST_REQUIRE(result.success);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(result.service_id, "get_timer_count",
                                         nlohmann::json::array());
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].get<int>() >= 2;
        },
        std::chrono::seconds(2)));
}

// Step 3: sync_call via CAF actor — manager->call() routes through actor.
BOOST_AUTO_TEST_CASE(SyncCallViaCafActor) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    // Spawn a messaging service that has a "echo" method.
    auto result = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                                opts_for("sync_call_target").dump());
    BOOST_REQUIRE(result.success);

    // manager.call() should route through the CAF actor and return result.
    CallResult cr =
        manager.call(result.service_id, "echo", R"(["hello"])"_json);
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE(cr.values.is_array());
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), "hello");
}

// Step 3: sync_call to nonexistent service returns error.
BOOST_AUTO_TEST_CASE(SyncCallToNonexistentService) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    CallResult cr =
        manager.call("nonexistent_service", "method", nlohmann::json::array());
    BOOST_CHECK(!cr.success);
    BOOST_CHECK(cr.error_message.find("not found") != std::string::npos);
}

// Step 3: sync_call to service without actor falls back to direct call.
BOOST_AUTO_TEST_CASE(SyncCallFallbackWithoutActor) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    // No attach_actor_system — no CAF actors.

    auto result = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                                opts_for("sync_fallback").dump());
    BOOST_REQUIRE(result.success);

    // Should use direct-call fallback (no CAF actor).
    CallResult cr =
        manager.call(result.service_id, "echo", R"(["world"])"_json);
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), "world");
}

BOOST_AUTO_TEST_SUITE_END()
