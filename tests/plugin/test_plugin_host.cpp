// Tests for PluginHost pipeline: scan/catalog/plan/resolve/load/create/start.
#define BOOST_TEST_MODULE shield_plugin_host
#include <boost/test/included/unit_test.hpp>

#include "shield/plugin/plugin_host.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace shield::plugin;

namespace {
const char* kMinimalManifest =
    R"({"schema_version":1,"id":"minimal.test","name":"Minimal","version":"1.0.0",)"
    R"("kind":"test","entry":"shield_plugin_get_v1",)"
    R"("library":{"linux":"bin/libshield_minimal_test_plugin.so",)"
    R"("macos":"bin/libshield_minimal_test_plugin.dylib","windows":"bin/shield_minimal_test_plugin.dll"},)"
    R"("provides":[],"requires":[],"config_schema":{"type":"object"}})";

// Writes a minimal package layout under a temp dir and returns the dir.
fs::path make_minimal_package() {
    fs::path root = fs::temp_directory_path() / "shield_plugin_host_test";
    fs::remove_all(root);
    fs::create_directories(root / "minimal.test" / "bin");
    std::ofstream(root / "minimal.test" / "plugin.json") << kMinimalManifest;
    return root;
}
}  // namespace

BOOST_AUTO_TEST_CASE(scan_finds_package) {
    auto root = make_minimal_package();
    PluginHost host;
    host.scan(root.string());
    auto ids = host.package_ids();
    BOOST_REQUIRE_EQUAL(ids.size(), 1u);
    BOOST_CHECK_EQUAL(ids[0], "minimal.test");
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(catalog_rejects_duplicate_id) {
    auto root = make_minimal_package();
    fs::create_directories(root / "minimal.test.dup");
    std::ofstream(root / "minimal.test.dup" / "plugin.json") << kMinimalManifest;
    PluginHost host;
    host.scan(root.string());
    std::string err;
    BOOST_CHECK(!host.catalog(err));
    BOOST_TEST(err.find("duplicate") != std::string::npos);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(plan_rejects_missing_package) {
    auto root = make_minimal_package();
    PluginHost host;
    host.scan(root.string());
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    InstanceDecl in;
    in.id = "x";
    in.package = "does.not.exist";
    in.required = true;
    cfg.instances.push_back(in);
    BOOST_CHECK(!host.plan_and_resolve(cfg, err));
    BOOST_TEST(err.find("plugin.package.not_found") != std::string::npos);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(resolve_detects_cycle) {
    // Two packages depending on each other.
    auto root = fs::temp_directory_path() / "shield_plugin_cycle_test";
    fs::remove_all(root);
    fs::create_directories(root / "a" / "bin");
    fs::create_directories(root / "b" / "bin");
    std::ofstream(root / "a" / "plugin.json") <<
        R"({"schema_version":1,"id":"a","entry":"shield_plugin_get_v1",)"
        R"("library":{"linux":"bin/a.so","macos":"bin/a.dylib","windows":"bin/a.dll"},)"
        R"("provides":[{"interface":"iface.a"}],)"
        R"("requires":[{"name":"b","interface":"iface.b","optional":false}],)"
        R"("config_schema":{"type":"object"}})";
    std::ofstream(root / "b" / "plugin.json") <<
        R"({"schema_version":1,"id":"b","entry":"shield_plugin_get_v1",)"
        R"("library":{"linux":"bin/b.so","macos":"bin/b.dylib","windows":"bin/b.dll"},)"
        R"("provides":[{"interface":"iface.b"}],)"
        R"("requires":[{"name":"a","interface":"iface.a","optional":false}],)"
        R"("config_schema":{"type":"object"}})";

    PluginHost host;
    host.scan(root.string());
    std::string err;
    BOOST_REQUIRE(host.catalog(err));
    PluginConfig cfg;
    cfg.directory = root.string();
    InstanceDecl ia; ia.id = "ia"; ia.package = "a"; ia.dependencies["b"] = "ib";
    InstanceDecl ib; ib.id = "ib"; ib.package = "b"; ib.dependencies["a"] = "ia";
    cfg.instances.push_back(ia);
    cfg.instances.push_back(ib);
    BOOST_CHECK(!host.plan_and_resolve(cfg, err));
    BOOST_TEST(err.find("plugin.dependency.cycle") != std::string::npos);
    fs::remove_all(root);
}
