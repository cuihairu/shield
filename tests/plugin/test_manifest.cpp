// Tests for plugin manifest parsing + validation.
#define BOOST_TEST_MODULE shield_plugin_manifest
#include <boost/test/included/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "shield/plugin/plugin_host.hpp"

using namespace shield::plugin;
using json = nlohmann::json;
namespace fs = std::filesystem;

BOOST_AUTO_TEST_CASE(parse_minimal_manifest) {
    json j = {
        {"schema_version", 1},
        {"id", "database.sqlite"},
        {"name", "SQLite"},
        {"version", "1.0.0"},
        {"kind", "database"},
        {"entry", "shield_plugin_get_v1"},
        {"library",
         {{"linux", "bin/libshield_database_sqlite.so"},
          {"macos", "bin/libshield_database_sqlite.dylib"},
          {"windows", "bin/libshield_database_sqlite.dll"}}},
        {"provides", json::array({{{"interface", "shield.database.v1"},
                                   {"capabilities", json::array({"sql"})}}})},
        {"requires", json::array()},
        {"config_schema", {{"type", "object"}}}};
    auto m = parse_manifest(j);
    BOOST_CHECK_EQUAL(m.id, "database.sqlite");
    BOOST_CHECK_EQUAL(m.entry, "shield_plugin_get_v1");
    BOOST_REQUIRE_EQUAL(m.provides.size(), 1u);
    BOOST_CHECK_EQUAL(m.provides[0].interface_name, "shield.database.v1");
    BOOST_REQUIRE_EQUAL(m.provides[0].capabilities.size(), 1u);
    BOOST_CHECK_EQUAL(m.provides[0].capabilities[0], "sql");
}

BOOST_AUTO_TEST_CASE(rejects_missing_id) {
    json j = {{"schema_version", 1},
              {"entry", "shield_plugin_get_v1"},
              {"library", {{"linux", "x.so"}}},
              {"provides", json::array()},
              {"requires", json::array()}};
    BOOST_CHECK_THROW(parse_manifest(j), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rejects_wrong_schema_version) {
    json j = {{"schema_version", 2},
              {"id", "x"},
              {"entry", "shield_plugin_get_v1"},
              {"library", {{"linux", "x.so"}}},
              {"provides", json::array()},
              {"requires", json::array()},
              {"config_schema", json::object()}};
    BOOST_CHECK_THROW(parse_manifest(j), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(parse_requires) {
    json j = {
        {"schema_version", 1},
        {"id", "lb.redis"},
        {"entry", "shield_plugin_get_v1"},
        {"library", {{"linux", "x.so"}}},
        {"provides", json::array({{{"interface", "shield.leaderboard.v1"}}})},
        {"requires", json::array({{{"name", "db"},
                                   {"interface", "shield.database.v1"},
                                   {"optional", false}}})},
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
    std::ofstream(path) << "schema_version: 1\n"
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

BOOST_AUTO_TEST_CASE(rejects_non_manifest_yaml_filename) {
    auto root = fs::temp_directory_path() / "shield_plugin_manifest_name_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto path = root / "plugin.yaml";
    std::ofstream(path) << "schema_version: 1\n"
                           "id: database.sqlite\n"
                           "entry: shield_plugin_get_v1\n"
                           "library:\n"
                           "  linux: bin/libshield_database_sqlite.so\n"
                           "  macos: bin/libshield_database_sqlite.dylib\n"
                           "  windows: bin/libshield_database_sqlite.dll\n"
                           "provides:\n"
                           "  - interface: shield.database.v1\n"
                           "requires: []\n"
                           "config_schema:\n"
                           "  type: object\n";

    BOOST_CHECK_THROW(load_manifest_file(path), std::runtime_error);
    fs::remove_all(root);
}

// Branch-hygiene cases for the optional-field arms of parse_manifest: wrong
// JSON types and explicit nulls take the "skip / treat as missing" path.
BOOST_AUTO_TEST_CASE(parse_manifest_optional_field_edge_arms) {
    // A present-but-null field counts as missing (require_field is_null arm).
    json null_id = {{"schema_version", 1},
                    {"id", json()},
                    {"entry", "shield_plugin_get_v1"},
                    {"library", {{"linux", "x.so"}}},
                    {"provides", json::array()},
                    {"requires", json::array()}};
    BOOST_CHECK_THROW(parse_manifest(null_id), std::runtime_error);

    // Non-array capabilities are skipped rather than fatal.
    json caps_scalar = {
        {"schema_version", 1},
        {"id", "x"},
        {"entry", "e"},
        {"library", {{"linux", "x.so"}}},
        {"provides",
         json::array({{{"interface", "i"}, {"capabilities", "oops"}}})},
        {"requires", json::array()}};
    auto caps = parse_manifest(caps_scalar);
    BOOST_REQUIRE_EQUAL(caps.provides.size(), 1u);
    BOOST_CHECK(caps.provides[0].capabilities.empty());

    // A non-array requires block is ignored entirely.
    json requires_scalar = {{"schema_version", 1},
                            {"id", "x"},
                            {"entry", "e"},
                            {"library", {{"linux", "x.so"}}},
                            {"provides", json::array({{{"interface", "i"}}})},
                            {"requires", "oops"}};
    auto reqs = parse_manifest(requires_scalar);
    BOOST_CHECK(reqs.requires_.empty());

    // A non-object lua block is ignored: lua stays disabled.
    json lua_scalar = {{"schema_version", 1},
                       {"id", "x"},
                       {"entry", "e"},
                       {"library", {{"linux", "x.so"}}},
                       {"provides", json::array({{{"interface", "i"}}})},
                       {"requires", json::array()},
                       {"lua", "oops"}};
    auto lua_bad = parse_manifest(lua_scalar);
    BOOST_CHECK(!lua_bad.lua.enabled);

    // A lua object whose search_paths is not an array leaves both "enabled"
    // inputs empty, so lua.enabled stays false (both-arms-empty path).
    json search_paths_scalar = {
        {"schema_version", 1},
        {"id", "x"},
        {"entry", "e"},
        {"library", {{"linux", "x.so"}}},
        {"provides", json::array({{{"interface", "i"}}})},
        {"requires", json::array()},
        {"lua", {{"search_paths", "oops"}}}};
    auto sp = parse_manifest(search_paths_scalar);
    BOOST_CHECK(sp.lua.namespace_.empty());
    BOOST_CHECK(sp.lua.search_paths.empty());
    BOOST_CHECK(!sp.lua.enabled);

    // A lua block with only search_paths enables lua via the second arm of
    // the "either non-empty" disjunction (namespace stays empty).
    json search_paths_only = {
        {"schema_version", 1},
        {"id", "x"},
        {"entry", "e"},
        {"library", {{"linux", "x.so"}}},
        {"provides", json::array({{{"interface", "i"}}})},
        {"requires", json::array()},
        {"lua", {{"search_paths", json::array({"scripts/?.lua"})}}}};
    auto spo = parse_manifest(search_paths_only);
    BOOST_CHECK(spo.lua.namespace_.empty());
    BOOST_CHECK(!spo.lua.search_paths.empty());
    BOOST_CHECK(spo.lua.enabled);

    // A non-object documentation block is ignored.
    json docs_scalar = {{"schema_version", 1},
                        {"id", "x"},
                        {"entry", "e"},
                        {"library", {{"linux", "x.so"}}},
                        {"provides", json::array({{{"interface", "i"}}})},
                        {"requires", json::array()},
                        {"documentation", "oops"}};
    auto doc_bad = parse_manifest(docs_scalar);
    BOOST_CHECK(!doc_bad.documentation.enabled);

    // documentation with a description but no url: description captured,
    // enabled stays false (the "url" key missing arm).
    json docs_desc_only = {{"schema_version", 1},
                           {"id", "x"},
                           {"entry", "e"},
                           {"library", {{"linux", "x.so"}}},
                           {"provides", json::array({{{"interface", "i"}}})},
                           {"requires", json::array()},
                           {"documentation", {{"description", "d"}}}};
    auto desc_only = parse_manifest(docs_desc_only);
    BOOST_CHECK_EQUAL(desc_only.documentation.description, "d");
    BOOST_CHECK(desc_only.documentation.url.empty());
    BOOST_CHECK(!desc_only.documentation.enabled);

    // documentation with a url but no description: enabled (the "description"
    // key missing arm).
    json docs_url_only = {
        {"schema_version", 1},
        {"id", "x"},
        {"entry", "e"},
        {"library", {{"linux", "x.so"}}},
        {"provides", json::array({{{"interface", "i"}}})},
        {"requires", json::array()},
        {"documentation", {{"url", "https://example.invalid/x"}}}};
    auto url_only = parse_manifest(docs_url_only);
    BOOST_CHECK_EQUAL(url_only.documentation.url, "https://example.invalid/x");
    BOOST_CHECK(url_only.documentation.description.empty());
    BOOST_CHECK(url_only.documentation.enabled);
}
