// LAPI-010: shield.config read API.
//
// Verifies that shield.config(key [, default]) reads flat keys from the global
// runtime config and applies the documented value parsing: "true"/"false" map
// to booleans, integer-looking strings to integers, float-looking strings to
// doubles, everything else to a string, and missing keys return the supplied
// default (or nil when none is given).
#define BOOST_TEST_MODULE LuaApiConfigTests
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "shield/config/config.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;
using shield::config::global_config;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

SpawnResult spawn_config_service(LuaServiceManager& manager,
                                 const std::string& name) {
    nlohmann::json opts = {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
    return manager.spawn(TEST_SCRIPTS_DIR + "config_service.lua", opts.dump());
}

// Read shield.config(key) through the spawned service and return the first
// JSON return value (or a null json if the call failed).
nlohmann::json read_value(LuaServiceManager& manager,
                          const std::string& service_id,
                          const std::string& key) {
    CallResult cr =
        manager.call(service_id, "read", nlohmann::json::array({key}), 1000);
    if (!cr.success || cr.values.empty()) {
        return nullptr;
    }
    return cr.values[0];
}

// Read shield.config(key, default) through the spawned service.
nlohmann::json read_default(LuaServiceManager& manager,
                            const std::string& service_id,
                            const std::string& key,
                            const nlohmann::json& default_value) {
    CallResult cr =
        manager.call(service_id, "read_default",
                     nlohmann::json::array({key, default_value}), 1000);
    if (!cr.success || cr.values.empty()) {
        return nullptr;
    }
    return cr.values[0];
}
}  // namespace

BOOST_AUTO_TEST_SUITE(Lapi010ConfigApi)

BOOST_AUTO_TEST_CASE(LAPI_010_01_ParsesTypedValues) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto& cfg = global_config();
    cfg.set("lapi_cfg.bool_true", std::string("true"));
    cfg.set("lapi_cfg.bool_false", std::string("false"));
    cfg.set("lapi_cfg.int", std::string("42"));
    cfg.set("lapi_cfg.neg_int", std::string("-7"));
    cfg.set("lapi_cfg.double", std::string("3.14"));
    cfg.set("lapi_cfg.string", std::string("hello"));
    cfg.set("lapi_cfg.not_a_number", std::string("12abc"));

    auto spawned = spawn_config_service(manager, "config_reader");
    BOOST_REQUIRE(spawned.success);

    const auto id = spawned.service_id;

    BOOST_CHECK_EQUAL(read_value(manager, id, "lapi_cfg.bool_true"), true);
    BOOST_CHECK_EQUAL(read_value(manager, id, "lapi_cfg.bool_false"), false);
    BOOST_CHECK_EQUAL(read_value(manager, id, "lapi_cfg.int"), 42);
    BOOST_CHECK_EQUAL(read_value(manager, id, "lapi_cfg.neg_int"), -7);
    BOOST_CHECK_CLOSE(read_value(manager, id, "lapi_cfg.double").get<double>(),
                      3.14, 1e-9);
    BOOST_CHECK_EQUAL(read_value(manager, id, "lapi_cfg.string"), "hello");
    BOOST_CHECK_EQUAL(read_value(manager, id, "lapi_cfg.not_a_number"),
                      "12abc");
}

BOOST_AUTO_TEST_CASE(LAPI_010_02_MissingKeyReturnsDefaultOrNil) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto spawned = spawn_config_service(manager, "config_reader_missing");
    BOOST_REQUIRE(spawned.success);

    const auto id = spawned.service_id;

    // Missing key without a default resolves to nil (null JSON).
    BOOST_CHECK(
        read_value(manager, id, "lapi_cfg.definitely_missing").is_null());

    // Missing key with a default resolves to the default value, preserving its
    // Lua type.
    BOOST_CHECK_EQUAL(
        read_default(manager, id, "lapi_cfg.definitely_missing", "fallback"),
        "fallback");
    BOOST_CHECK_EQUAL(
        read_default(manager, id, "lapi_cfg.definitely_missing", 99), 99);
    BOOST_CHECK_EQUAL(
        read_default(manager, id, "lapi_cfg.definitely_missing", true), true);
}

BOOST_AUTO_TEST_CASE(LAPI_010_03_ExplicitValueShadowsDefault) {
    caf::actor_system_config cfg;

    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto& cfg = global_config();
    cfg.set("lapi_cfg.present", std::string("ok"));

    auto spawned = spawn_config_service(manager, "config_reader_shadow");
    BOOST_REQUIRE(spawned.success);

    const auto id = spawned.service_id;

    // A stored value must be returned even when a default is supplied.
    BOOST_CHECK_EQUAL(
        read_default(manager, id, "lapi_cfg.present", "unused_default"), "ok");
}

BOOST_AUTO_TEST_SUITE_END()
