#define BOOST_TEST_MODULE LuaVMPoolConfigTest
#include <boost/test/unit_test.hpp>
#include <stdexcept>

#include "shield/script/lua_vm_pool_config.hpp"

using namespace shield::script;

BOOST_AUTO_TEST_SUITE(LuaVMPoolConfigPropertiesTests)

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    LuaVMPoolConfigProperties config;
    BOOST_CHECK_EQUAL(config.initial_size, 4u);
    BOOST_CHECK_EQUAL(config.max_size, 16u);
    BOOST_CHECK_EQUAL(config.min_size, 2u);
    BOOST_CHECK_EQUAL(config.idle_timeout_ms, 30000);
    BOOST_CHECK_EQUAL(config.acquire_timeout_ms, 5000);
    BOOST_CHECK(config.preload_scripts);
    BOOST_CHECK(config.script_paths.empty());
}

BOOST_AUTO_TEST_CASE(TestPropertiesName) {
    LuaVMPoolConfigProperties config;
    BOOST_CHECK_EQUAL(config.properties_name(), "lua_vm_pool");
}

BOOST_AUTO_TEST_CASE(TestSupportsHotReload) {
    LuaVMPoolConfigProperties config;
    BOOST_CHECK(config.supports_hot_reload());
}

BOOST_AUTO_TEST_CASE(TestValidateValid) {
    LuaVMPoolConfigProperties config;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateMinMaxSwap) {
    LuaVMPoolConfigProperties config;
    config.min_size = 10;
    config.max_size = 5;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateInitialBelowMin) {
    LuaVMPoolConfigProperties config;
    config.min_size = 5;
    config.initial_size = 3;
    config.max_size = 10;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateInitialAboveMax) {
    LuaVMPoolConfigProperties config;
    config.min_size = 2;
    config.initial_size = 20;
    config.max_size = 10;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroIdleTimeout) {
    LuaVMPoolConfigProperties config;
    config.idle_timeout_ms = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroAcquireTimeout) {
    LuaVMPoolConfigProperties config;
    config.acquire_timeout_ms = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestToPoolConfig) {
    LuaVMPoolConfigProperties props;
    props.initial_size = 4;
    props.max_size = 16;
    props.min_size = 2;
    props.idle_timeout_ms = 30000;
    props.acquire_timeout_ms = 5000;
    props.preload_scripts = false;

    auto cfg = props.to_pool_config();
    BOOST_CHECK_EQUAL(cfg.initial_size, 4u);
    BOOST_CHECK_EQUAL(cfg.max_size, 16u);
    BOOST_CHECK_EQUAL(cfg.min_size, 2u);
    BOOST_CHECK_EQUAL(cfg.idle_timeout.count(), 30000);
    BOOST_CHECK_EQUAL(cfg.acquire_timeout.count(), 5000);
    BOOST_CHECK(!cfg.preload_scripts);
}

BOOST_AUTO_TEST_CASE(TestFromPtree) {
    boost::property_tree::ptree pt;
    pt.put("initial_size", 8);
    pt.put("min_size", 4);
    pt.put("max_size", 32);
    pt.put("idle_timeout_ms", 60000);
    pt.put("acquire_timeout_ms", 10000);
    pt.put("preload_scripts", false);

    LuaVMPoolConfigProperties config;
    config.from_ptree(pt);

    BOOST_CHECK_EQUAL(config.initial_size, 8u);
    BOOST_CHECK_EQUAL(config.min_size, 4u);
    BOOST_CHECK_EQUAL(config.max_size, 32u);
    BOOST_CHECK_EQUAL(config.idle_timeout_ms, 60000);
    BOOST_CHECK_EQUAL(config.acquire_timeout_ms, 10000);
    BOOST_CHECK(!config.preload_scripts);
}

BOOST_AUTO_TEST_CASE(TestFromPtreeWithScriptPaths) {
    boost::property_tree::ptree pt;
    boost::property_tree::ptree paths;
    boost::property_tree::ptree p1, p2;
    p1.put("", "scripts/init.lua");
    p2.put("", "scripts/game.lua");
    paths.push_back(std::make_pair("", p1));
    paths.push_back(std::make_pair("", p2));
    pt.add_child("script_paths", paths);

    LuaVMPoolConfigProperties config;
    config.from_ptree(pt);

    BOOST_REQUIRE_EQUAL(config.script_paths.size(), 2u);
    BOOST_CHECK_EQUAL(config.script_paths[0], "scripts/init.lua");
    BOOST_CHECK_EQUAL(config.script_paths[1], "scripts/game.lua");
}

BOOST_AUTO_TEST_SUITE_END()
