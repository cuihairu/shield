#define BOOST_TEST_MODULE LuaApiCafBridgeTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <nlohmann/json.hpp>
#include <string>

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

    // send still works
    auto s = manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                           opts_for("caf_api_echo").dump());
    BOOST_REQUIRE(s.success);
    BOOST_CHECK(
        manager.send("caf_api_echo", "echo", nlohmann::json::array({"hello"})));

    // call still works
    CallResult cr =
        manager.call(s.service_id, "echo", nlohmann::json::array({"world"}));
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), "world");
}

BOOST_AUTO_TEST_SUITE_END()
