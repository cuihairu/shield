#define BOOST_TEST_MODULE LuaApiLegacyTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <nlohmann/json.hpp>
#include <string>

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

SpawnResult spawn_legacy(LuaServiceManager& manager, const std::string& name) {
    return manager.spawn(TEST_SCRIPTS_DIR + "legacy_api_service.lua",
                         opts_for(name).dump());
}
}  // namespace

BOOST_AUTO_TEST_SUITE(LegacyApiTests)

BOOST_AUTO_TEST_CASE(LAPI_010_01_OldServiceApiUnavailable) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_legacy(manager, "legacy_service_test");
    BOOST_REQUIRE(service.success);

    CallResult cr = manager.call(service.service_id, "has_service_api",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(LAPI_010_02_PluginApiAvailable) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_legacy(manager, "legacy_plugin_test");
    BOOST_REQUIRE(service.success);

    // shield.plugin is now a valid API (not legacy).
    CallResult cr = manager.call(service.service_id, "has_plugin_api",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(LAPI_010_02A_NewPluginIntrospectionAvailable) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_legacy(manager, "legacy_plugin_v1_test");
    BOOST_REQUIRE(service.success);

    CallResult cr = manager.call(service.service_id,
                                 "has_new_plugin_introspection",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(LAPI_010_03_OldColonDbApiFails) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_legacy(manager, "legacy_db_test");
    BOOST_REQUIRE(service.success);

    CallResult cr = manager.call(service.service_id, "colon_db_call_fails",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(LAPI_010_04_OldOnMessageEntryIsNotDispatched) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_legacy(manager, "legacy_onmessage_test");
    BOOST_REQUIRE(service.success);

    BOOST_REQUIRE(manager.send(service.service_id, "test_method",
                               nlohmann::json::array({"payload"})));
    BOOST_REQUIRE(manager.process_mailbox(service.service_id));

    CallResult cr = manager.call(service.service_id, "on_message_called",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), false);
}

BOOST_AUTO_TEST_CASE(LAPI_010_05_OldDiApiUnavailable) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_legacy(manager, "legacy_di_test");
    BOOST_REQUIRE(service.success);

    CallResult cr = manager.call(service.service_id, "has_di_api",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), false);
}

BOOST_AUTO_TEST_SUITE_END()
