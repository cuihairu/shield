#define BOOST_TEST_MODULE CovManifest
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "shield/plugin/plugin_host.hpp"

using namespace shield::plugin;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json base_manifest() {
    return json{
        {"schema_version", 1},
        {"id", "cov.plugin"},
        {"entry", "shield_plugin_get_v1"},
        {"library", {{"linux", "cov.so"}}},
        {"provides", json::array({json{{"interface", "cov.iface.v1"}}})}};
}

// Creates <tmp>/shield_cov_manifest_<tag>/manifest.yaml with `content` and
// returns the file path.
fs::path write_manifest(const std::string& tag, const std::string& content) {
    auto root = fs::temp_directory_path() / ("shield_cov_manifest_" + tag);
    fs::remove_all(root);
    fs::create_directories(root);
    std::ofstream(root / "manifest.yaml") << content;
    return root / "manifest.yaml";
}

}  // namespace

// Optional lua metadata: namespace plus string search_paths entries
// (non-string entries are skipped) and the derived enabled flag.
BOOST_AUTO_TEST_CASE(parse_lua_metadata) {
    auto j = base_manifest();
    j["lua"] = {{"namespace", "database.mongodb"},
                {"search_paths", json::array({"lua/?.lua", 42})}};
    auto m = parse_manifest(j);
    BOOST_CHECK_EQUAL(m.lua.namespace_, "database.mongodb");
    BOOST_REQUIRE_EQUAL(m.lua.search_paths.size(), 1u);
    BOOST_CHECK_EQUAL(m.lua.search_paths[0], "lua/?.lua");
    BOOST_CHECK(m.lua.enabled);
}

// An empty lua object leaves enabled false; a non-string namespace is
// ignored, keeping enabled false as well.
BOOST_AUTO_TEST_CASE(parse_lua_metadata_disabled_variants) {
    auto j = base_manifest();
    j["lua"] = json::object();
    auto m = parse_manifest(j);
    BOOST_CHECK(m.lua.namespace_.empty());
    BOOST_CHECK(m.lua.search_paths.empty());
    BOOST_CHECK(!m.lua.enabled);

    auto j2 = base_manifest();
    j2["lua"] = {{"namespace", nullptr}};
    auto m2 = parse_manifest(j2);
    BOOST_CHECK(m2.lua.namespace_.empty());
    BOOST_CHECK(!m2.lua.enabled);
}

// Optional documentation metadata: url + description, with enabled derived
// from the url; non-string / missing fields stay empty.
BOOST_AUTO_TEST_CASE(parse_documentation_metadata) {
    auto j = base_manifest();
    j["documentation"] = {{"url", "https://example.com/docs"},
                          {"description", "cov docs"}};
    auto m = parse_manifest(j);
    BOOST_CHECK_EQUAL(m.documentation.url, "https://example.com/docs");
    BOOST_CHECK_EQUAL(m.documentation.description, "cov docs");
    BOOST_CHECK(m.documentation.enabled);

    // No url -> disabled; non-string url/description are ignored.
    auto j2 = base_manifest();
    j2["documentation"] = {{"url", 7}, {"description", 7}};
    auto m2 = parse_manifest(j2);
    BOOST_CHECK(m2.documentation.url.empty());
    BOOST_CHECK(m2.documentation.description.empty());
    BOOST_CHECK(!m2.documentation.enabled);
}

// Full happy path through load_manifest_file: exercises yaml_to_json scalar
// coercion (bool/int/double/string), sequences and maps.
BOOST_AUTO_TEST_CASE(load_yaml_file_with_lua_and_docs) {
    auto path = write_manifest("full",
                               "schema_version: 1\n"
                               "id: cov.yaml\n"
                               "entry: shield_plugin_get_v1\n"
                               "library:\n"
                               "  linux: cov.so\n"
                               "provides:\n"
                               "  - interface: cov.iface.v1\n"
                               "    capabilities: [sql]\n"
                               "lua:\n"
                               "  namespace: cov.lua\n"
                               "  search_paths: [\"lua/?.lua\"]\n"
                               "documentation:\n"
                               "  url: https://example.com/cov\n"
                               "  description: desc\n"
                               "config_schema:\n"
                               "  type: object\n"
                               "  properties:\n"
                               "    port:\n"
                               "      type: integer\n"
                               "      minimum: 1\n"
                               "      maximum: 65535\n"
                               "    flag: true\n"
                               "    ratio: 1.5\n"
                               "    label: hello\n");
    auto m = load_manifest_file(path);
    BOOST_CHECK_EQUAL(m.id, "cov.yaml");
    BOOST_CHECK_EQUAL(m.lua.namespace_, "cov.lua");
    BOOST_CHECK(m.lua.enabled);
    BOOST_CHECK_EQUAL(m.documentation.url, "https://example.com/cov");
    BOOST_CHECK(m.documentation.enabled);
    BOOST_CHECK_EQUAL(platform_library_path(m), "cov.so");
    BOOST_CHECK_EQUAL(m.config_schema["properties"]["label"].get<std::string>(),
                      "hello");
    fs::remove_all(path.parent_path());
}

// An empty manifest.yaml is a null YAML document: yaml_to_json yields null
// and parse_manifest rejects it for the missing schema_version.
BOOST_AUTO_TEST_CASE(load_empty_manifest_file_throws) {
    auto path = write_manifest("empty", "");
    BOOST_CHECK_THROW(load_manifest_file(path), std::runtime_error);
    fs::remove_all(path.parent_path());
}

// Unopenable path (directory exists, file does not).
BOOST_AUTO_TEST_CASE(load_manifest_missing_file_throws) {
    auto root = fs::temp_directory_path() / "shield_cov_manifest_missing";
    fs::remove_all(root);
    fs::create_directories(root);
    BOOST_CHECK_THROW(load_manifest_file(root / "manifest.yaml"),
                      std::runtime_error);
    fs::remove_all(root);
}

// Malformed YAML is converted from YAML::ParserException.
BOOST_AUTO_TEST_CASE(load_manifest_yaml_syntax_error_throws) {
    auto path = write_manifest("syntax", "key: [1, 2\n");
    try {
        load_manifest_file(path);
        BOOST_FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK(std::string(e.what()).find("YAML parse error") !=
                    std::string::npos);
    }
    fs::remove_all(path.parent_path());
}

// A complex (non-scalar) map key fails string conversion: YAML::BadConversion
// is rethrown as std::runtime_error.
BOOST_AUTO_TEST_CASE(load_manifest_bad_conversion_throws) {
    auto path = write_manifest("badconv", "? [foo, bar]\n: baz\n");
    try {
        load_manifest_file(path);
        BOOST_FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK(std::string(e.what()).find("YAML conversion error") !=
                    std::string::npos);
    }
    fs::remove_all(path.parent_path());
}

// platform_library_path returns the current platform's library entry.
BOOST_AUTO_TEST_CASE(platform_library_path_picks_linux) {
    auto j = base_manifest();
    j["library"] = {
        {"linux", "l.so"}, {"macos", "m.dylib"}, {"windows", "w.dll"}};
    auto m = parse_manifest(j);
    BOOST_CHECK_EQUAL(platform_library_path(m), "l.so");
}

// schema_version other than 1 is rejected with a runtime_error.
BOOST_AUTO_TEST_CASE(load_manifest_wrong_schema_version_throws) {
    auto path = write_manifest("wrongver", "schema_version: 2\n");
    try {
        load_manifest_file(path);
        BOOST_FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK(std::string(e.what()).find("schema_version must be 1") !=
                    std::string::npos);
    }
    fs::remove_all(path.parent_path());
}

// load_manifest_file refuses files that are not named manifest.yaml.
BOOST_AUTO_TEST_CASE(load_manifest_wrong_file_name_throws) {
    auto root = fs::temp_directory_path() / "shield_cov_manifest_wrongname";
    fs::remove_all(root);
    fs::create_directories(root);
    auto path = root / "plugin.yaml";
    std::ofstream(path) << "schema_version: 1\n";
    try {
        load_manifest_file(path);
        BOOST_FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& e) {
        BOOST_CHECK(std::string(e.what()).find("must be named manifest.yaml") !=
                    std::string::npos);
    }
    fs::remove_all(root);
}
