// Tests for plugin manifest parsing + validation.
#define BOOST_TEST_MODULE shield_plugin_manifest
#include <boost/test/included/unit_test.hpp>

#include "shield/plugin/plugin_host.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace shield::plugin;
using json = nlohmann::json;
namespace fs = std::filesystem;

BOOST_AUTO_TEST_CASE(parse_minimal_manifest) {
    json j = {
        {"schema_version", 1}, {"id", "database.sqlite"},
        {"name", "SQLite"}, {"version", "1.0.0"}, {"kind", "database"},
        {"entry", "shield_plugin_get_v1"},
        {"library", {{"linux", "bin/libshield_database_sqlite.so"},
                     {"macos", "bin/libshield_database_sqlite.dylib"},
                     {"windows", "bin/libshield_database_sqlite.dll"}}},
        {"provides", json::array({{{"interface", "shield.database.v1"},
                                   {"capabilities", json::array({"sql"})}}})},
        {"requires", json::array()},
        {"config_schema", {{"type", "object"}}}
    };
    auto m = parse_manifest(j);
    BOOST_CHECK_EQUAL(m.id, "database.sqlite");
    BOOST_CHECK_EQUAL(m.entry, "shield_plugin_get_v1");
    BOOST_REQUIRE_EQUAL(m.provides.size(), 1u);
    BOOST_CHECK_EQUAL(m.provides[0].interface_name, "shield.database.v1");
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
    BOOST_CHECK_EQUAL(m.requires_[0].interface_name, "shield.database.v1");
    BOOST_CHECK_EQUAL(m.requires_[0].optional, false);
}

BOOST_AUTO_TEST_CASE(load_yaml_manifest_file) {
    auto root = fs::temp_directory_path() / "shield_plugin_manifest_yaml_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto path = root / "manifest.yaml";
    std::ofstream(path) <<
        "schema_version: 1\n"
        "id: database.sqlite\n"
        "name: SQLite\n"
        "version: 1.0.0\n"
        "kind: database\n"
        "entry: shield_plugin_get_v1\n"
        "library:\n"
        "  linux: bin/libshield_database_sqlite.so\n"
        "  macos: bin/libshield_database_sqlite.dylib\n"
        "  windows: bin/libshield_database_sqlite.dll\n"
        "provides:\n"
        "  - interface: shield.database.v1\n"
        "    capabilities: [sql]\n"
        "requires: []\n"
        "config_schema:\n"
        "  type: object\n";

    auto m = load_manifest_file(path);
    BOOST_CHECK_EQUAL(m.id, "database.sqlite");
    BOOST_REQUIRE_EQUAL(m.provides.size(), 1u);
    BOOST_CHECK_EQUAL(m.provides[0].interface_name, "shield.database.v1");
    fs::remove_all(root);
}
