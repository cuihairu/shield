#define BOOST_TEST_MODULE LuaApiRegistryTests
#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json opts_for(const std::string& name,
                        nlohmann::json config = nlohmann::json::object()) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", std::move(config)},
    };
}

SpawnResult spawn_service(LuaServiceManager& manager, const std::string& name,
                          nlohmann::json config = nlohmann::json::object()) {
    return manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                         opts_for(name, std::move(config)).dump());
}
}  // namespace

BOOST_AUTO_TEST_SUITE(RegistryTests)

BOOST_AUTO_TEST_CASE(LAPI_003_01_QueryByNameReturnsEquivalentHandle) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_service(manager, "test_query_service");
    BOOST_REQUIRE(result.success);

    BOOST_CHECK_EQUAL(manager.query_service("test_query_service"),
                      result.service_id);

    CallResult cr = manager.call(result.service_id, "query_id",
                                 nlohmann::json::array({"test_query_service"}));
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), result.service_id);
}

BOOST_AUTO_TEST_CASE(LAPI_003_02_DuplicateNameConflict) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto first = spawn_service(manager, "duplicate_test");
    BOOST_REQUIRE(first.success);

    auto second = spawn_service(manager, "duplicate_test");
    BOOST_CHECK(!second.success);
    BOOST_CHECK(second.error_message.find("already exists") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_003_03_InitFailedNameRollback) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    nlohmann::json config = {{"test_case", "return_false"}};
    auto result = manager.spawn(TEST_SCRIPTS_DIR + "lifecycle_service.lua",
                                opts_for("failed_init_service", config).dump());
    BOOST_CHECK(!result.success);
    BOOST_CHECK(manager.query_service("failed_init_service").empty());
}

BOOST_AUTO_TEST_CASE(LAPI_003_04_RegisterAlias) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_service(manager, "alias_owner",
                                {{"register_alias", "alias.visible"}});
    BOOST_REQUIRE(result.success);

    BOOST_CHECK_EQUAL(manager.query_service("alias_owner"), result.service_id);
    BOOST_CHECK_EQUAL(manager.query_service("alias.visible"),
                      result.service_id);

    CallResult cr = manager.call(result.service_id, "query_id",
                                 nlohmann::json::array({"alias.visible"}));
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), result.service_id);
}

BOOST_AUTO_TEST_CASE(LAPI_003_05_ServiceExitRemovesOwnedNames) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_service(manager, "exit_test_service",
                                {{"register_alias", "exit.alias"}});
    BOOST_REQUIRE(result.success);

    manager.exit(result.service_id, "test_exit");

    BOOST_CHECK(manager.query_service("exit_test_service").empty());
    BOOST_CHECK(manager.query_service("exit.alias").empty());
}

BOOST_AUTO_TEST_CASE(LAPI_003_06_InvalidNameRejected) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    for (const std::string invalid_name : {
             std::string(" "),
             std::string("name with spaces"),
             std::string("shield.reserved"),
             std::string(65, 'a'),
         }) {
        auto result = spawn_service(manager, invalid_name);
        BOOST_TEST_CONTEXT("invalid_name=" << invalid_name) {
            BOOST_CHECK(!result.success);
            BOOST_CHECK(result.error_message.find("invalid service name") !=
                        std::string::npos);
        }
    }

    auto owner = spawn_service(manager, "invalid_alias_owner");
    BOOST_REQUIRE(owner.success);

    CallResult cr = manager.call(owner.service_id, "register_name",
                                 nlohmann::json::array({"bad alias"}));
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 3u);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(cr.values[1].get<std::string>(), "register_failed");
}

BOOST_AUTO_TEST_CASE(LAPI_003_07_NamesApiListsPublishedNames) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    BOOST_CHECK(manager.list_services().empty());

    auto first = spawn_service(manager, "service_1");
    auto second = manager.spawn(TEST_SCRIPTS_DIR + "lifecycle_service.lua",
                                opts_for("service_2").dump());
    BOOST_REQUIRE(first.success);
    BOOST_REQUIRE(second.success);

    auto names = manager.list_services();
    BOOST_CHECK(std::find(names.begin(), names.end(), "service_1") !=
                names.end());
    BOOST_CHECK(std::find(names.begin(), names.end(), "service_2") !=
                names.end());

    CallResult cr = manager.call(first.service_id, "names_snapshot",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK(cr.values[0].is_array());
}

BOOST_AUTO_TEST_CASE(LAPI_003_08_UnregisterName) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result = spawn_service(manager, "unregister_test",
                                {{"register_alias", "unregister.alias"}});
    BOOST_REQUIRE(result.success);
    BOOST_CHECK_EQUAL(manager.query_service("unregister.alias"),
                      result.service_id);

    CallResult cr = manager.call(result.service_id, "unregister_name",
                                 nlohmann::json::array({"unregister.alias"}));
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 3u);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), true);
    BOOST_CHECK(cr.values[1].is_null());
    BOOST_CHECK(cr.values[2].is_null());
    BOOST_CHECK(manager.query_service("unregister.alias").empty());

    cr = manager.call(result.service_id, "unregister_name",
                      nlohmann::json::array({"unregister.alias"}));
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(cr.values[1].get<std::string>(), "unregister_failed");
}

BOOST_AUTO_TEST_SUITE_END()
