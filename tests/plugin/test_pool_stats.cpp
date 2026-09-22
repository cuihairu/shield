// Tests for PluginHost::collect_pool_stats (shield.pool.stats.v1).
#define BOOST_TEST_MODULE shield_plugin_pool_stats
#include <boost/test/included/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "shield/plugin/plugin_host.hpp"

namespace fs = std::filesystem;
using namespace shield::plugin;

namespace {

std::string minimal_library_name() {
#ifdef SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY
    return SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY;
#else
    return "libshield_minimal_test_plugin.so";
#endif
}

// Config schema shared by the stats-declaring manifests: every behavior
// switch the fixture understands.
const char* kStatsConfigSchema =
    R"(config_schema:
  type: object
  properties:
    start_fail:
      type: boolean
    stats_unavailable:
      type: boolean
    stats_unsupported:
      type: boolean
    stats_unknown_rc:
      type: boolean
    stats_error:
      type: boolean
    stats_vtable_null:
      type: boolean
    stats_vtable_short:
      type: boolean
    stats_no_fn:
      type: boolean
)";

std::string make_manifest(bool with_pool_stats) {
    std::string manifest =
        "schema_version: 1\n"
        "id: minimal.test\n"
        "name: Minimal\n"
        "version: 1.0.0\n"
        "kind: test\n"
        "entry: shield_plugin_get_v1\n"
        "library:\n"
        "  linux: bin/" +
        minimal_library_name() +
        "\n"
        "  macos: bin/" +
        minimal_library_name() +
        "\n"
        "  windows: bin/" +
        minimal_library_name() +
        "\n"
        "provides:\n"
        "  - interface: minimal.test.iface\n";
    if (with_pool_stats) {
        manifest += "  - interface: shield.pool.stats.v1\n";
    }
    manifest += "requires: []\n";
    manifest += kStatsConfigSchema;
    return manifest;
}

// Deploys one minimal-plugin package under a temp root and removes the root
// when destroyed. Declared BEFORE the PluginHost in each test so the tree
// outlives the host (the host holds the loaded library).
struct PackageDir {
    fs::path root;
    PackageDir(const std::string& dir, bool with_pool_stats) {
        root = fs::temp_directory_path() / "shield_pool_stats_test";
        fs::remove_all(root);
        fs::create_directories(root / dir / "bin");
        std::ofstream(root / dir / "manifest.yaml")
            << make_manifest(with_pool_stats);
        fs::copy_file(fs::path(SHIELD_TEST_PLUGINS_DIR) / "minimal.test" /
                          "bin" / minimal_library_name(),
                      root / dir / "bin" / minimal_library_name(),
                      fs::copy_options::overwrite_existing);
    }
    ~PackageDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// One started instance wired to the deployed package. `config` carries the
// fixture behavior switches.
void add_instance(PluginConfig& cfg, const std::string& id,
                  const std::string& package, nlohmann::json config = {}) {
    InstanceDecl decl;
    decl.id = id;
    decl.package = package;
    decl.config = std::move(config);
    cfg.instances.push_back(std::move(decl));
}

}  // namespace

BOOST_AUTO_TEST_CASE(collects_declared_instance_with_ok_stats) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test");

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_TEST(out[0].instance_id == "stats.inst");
    BOOST_TEST(out[0].plugin_id == "minimal.test");
    BOOST_TEST(out[0].pool_name == "main");
    BOOST_TEST(static_cast<int>(out[0].status) ==
               static_cast<int>(PoolStatsStatus::ok));
    BOOST_TEST(out[0].raw_status_code == 0);
    BOOST_TEST(out[0].error_code.empty());
    BOOST_TEST(out[0].error_message.empty());
    BOOST_TEST(out[0].stats.struct_size == sizeof(shield_pool_stats));
    BOOST_TEST(out[0].stats.max_size == 4);
    BOOST_TEST(out[0].stats.size == 2);
    BOOST_TEST(out[0].stats.idle == 1);
    BOOST_TEST(out[0].stats.in_use == 1);
    BOOST_TEST(out[0].stats.waiters == 0);
    BOOST_TEST(out[0].stats.acquire_total == 10);
    BOOST_TEST(out[0].stats.create_total == 2);
}

BOOST_AUTO_TEST_CASE(unavailable_return_code_maps_to_status) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test",
                 {{"stats_unavailable", true}});

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_TEST(static_cast<int>(out[0].status) ==
               static_cast<int>(PoolStatsStatus::unavailable));
    BOOST_TEST(out[0].raw_status_code == 1);
    BOOST_TEST(out[0].error_code == "pool_stats_unavailable");
    BOOST_TEST(out[0].error_message == "pool stats temporarily unavailable");
    // The plugin returned before touching any field: the host sentinel must
    // survive (all gauges/counters read as unknown).
    BOOST_TEST(out[0].stats.max_size == -1);
    BOOST_TEST(out[0].stats.acquire_total == -1);
}

BOOST_AUTO_TEST_CASE(unsupported_return_code_maps_to_status) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test",
                 {{"stats_unsupported", true}});

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_TEST(static_cast<int>(out[0].status) ==
               static_cast<int>(PoolStatsStatus::unsupported_state));
    BOOST_TEST(out[0].raw_status_code == 2);
    BOOST_TEST(out[0].error_code == "pool_stats_unsupported_state");
    BOOST_TEST(out[0].error_message.empty());
}

BOOST_AUTO_TEST_CASE(unknown_positive_rc_folds_to_unsupported) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test",
                 {{"stats_unknown_rc", true}});

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    // Unrecognized positive codes fold to unsupported_state, but the raw
    // value is preserved for future-extension diagnostics.
    BOOST_TEST(static_cast<int>(out[0].status) ==
               static_cast<int>(PoolStatsStatus::unsupported_state));
    BOOST_TEST(out[0].raw_status_code == 7);
    BOOST_TEST(out[0].error_code == "pool_stats_unsupported_state");
}

BOOST_AUTO_TEST_CASE(error_rc_maps_to_internal_error) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test", {{"stats_error", true}});

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_TEST(static_cast<int>(out[0].status) ==
               static_cast<int>(PoolStatsStatus::error));
    BOOST_TEST(out[0].raw_status_code == -1);
    BOOST_TEST(out[0].error_code == "pool_stats_internal_error");
    BOOST_TEST(out[0].error_message == "pool stats collection failed");
}

BOOST_AUTO_TEST_CASE(undeclared_instance_is_not_collected) {
    // The fixture still SERVES the vtable; the manifest is authoritative for
    // discovery, so the instance must not appear in the results.
    PackageDir pkg("nostats.test", false);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "nostats.inst", "minimal.test");

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_TEST(out.empty());
}

BOOST_AUTO_TEST_CASE(short_vtable_is_skipped) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test",
                 {{"stats_vtable_short", true}});

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_TEST(out.empty());
}

BOOST_AUTO_TEST_CASE(null_get_stats_is_skipped) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test", {{"stats_no_fn", true}});

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_TEST(out.empty());
}

BOOST_AUTO_TEST_CASE(declared_but_missing_vtable_fails_start) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "stats.inst", "minimal.test",
                 {{"stats_vtable_null", true}});

    // Manifest declares shield.pool.stats.v1 but get_interface returns NULL:
    // contract violation. The create phase enforces declared-interface
    // consistency for every provides entry (strictly earlier than start),
    // so startup fails fast there.
    BOOST_TEST(!host.startup(cfg, err));
    BOOST_TEST(err.find("does not provide declared interface "
                        "'shield.pool.stats.v1'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(failed_optional_instance_is_skipped) {
    PackageDir pkg("stats.test", true);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = pkg.root.string();
    add_instance(cfg, "good.inst", "minimal.test");
    InstanceDecl bad;
    bad.id = "bad.inst";
    bad.package = "minimal.test";
    bad.required = false;  // optional: startup succeeds despite the failure
    bad.config = {{"start_fail", true}};
    cfg.instances.push_back(bad);

    // Optional instance fails to start (state != started, handle released);
    // collection skips it and still returns the healthy one.
    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_REQUIRE_EQUAL(out.size(), 1u);
    BOOST_TEST(out[0].instance_id == "good.inst");
}
