// Tests for plugin.json manifest parsing + validation.
#define BOOST_TEST_MODULE shield_plugin_manifest
#include <boost/test/included/unit_test.hpp>

#include "shield/plugin/plugin_host.hpp"

#include <nlohmann/json.hpp>

using namespace shield::plugin;
using json = nlohmann::json;

BOOST_AUTO_TEST_CASE(parse_minimal_manifest) {
    json j = {
        {"schema_version", 1}, {"id", "database.sqlite"},
        {"name", "SQLite"}, {"version", "1.0.0"}, {"kind", "database"},
        {"entry", "shield_plugin_get_v1"},
        {"library", {{"linux", "bin/libshield_database_sqlite.so"},
                     {"macos", "bin/libshield_database_sqlite.dylib"},
                     {"windows", "bin/shield_database_sqlite.dll"}}},
        {"provides", json::array({{{"interface", "shield.database.v1"},
                                   {"capabilities", json::array({"sql"})}}})},
        {"requires", json::array()},
        {"config_schema", {{"type", "object"}}}
    };
    auto m = parse_manifest(j);
    BOOST_CHECK_EQUAL(m.id, "database.sqlite");
    BOOST_CHECK_EQUAL(m.entry, "shield_plugin_get_v1");
    BOOST_REQUIRE_EQUAL(m.provides.size(), 1u);
    BOOST_CHECK_EQUAL(m.provides[0].interface, "shield.database.v1");
    BOOST_REQUIRE_EQUAL(m.provides[0].capabilities.size(), 1u);
    BOOST_CHECK_EQUAL(m.provides[0].capabilities[0], "sql");
}

BOOST_AUTO_TEST_CASE(rejects_missing_id) {
    json j = {{"schema_version", 1}, {"entry", "shield_plugin_get_v1"},
              {"library", {{"linux", "x.so"}}}, {"provides", json::array()},
              {"requires", json::array()}};
    BOOST_CHECK_THROW(parse_manifest(j), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rejects_wrong_schema_version) {
    json j = {{"schema_version", 2}, {"id", "x"}, {"entry", "shield_plugin_get_v1"},
              {"library", {{"linux", "x.so"}}}, {"provides", json::array()},
              {"requires", json::array()}, {"config_schema", json::object()}};
    BOOST_CHECK_THROW(parse_manifest(j), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(parse_requires) {
    json j = {{"schema_version", 1}, {"id", "lb.redis"}, {"entry", "shield_plugin_get_v1"},
              {"library", {{"linux", "x.so"}}},
              {"provides", json::array({{{"interface", "shield.leaderboard.v1"}}})},
              {"requires", json::array({{{"name", "db"}, {"interface", "shield.database.v1"}, {"optional", false}}})},
              {"config_schema", json::object()}};
    auto m = parse_manifest(j);
    BOOST_REQUIRE_EQUAL(m.requires_.size(), 1u);
    BOOST_CHECK_EQUAL(m.requires_[0].name, "db");
    BOOST_CHECK_EQUAL(m.requires_[0].interface, "shield.database.v1");
    BOOST_CHECK_EQUAL(m.requires_[0].optional, false);
}
