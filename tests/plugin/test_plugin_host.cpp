// Tests for PluginHost pipeline: scan/catalog/plan/resolve/load/create/start.
#define BOOST_TEST_MODULE shield_plugin_host
#include <boost/test/included/unit_test.hpp>

#include "shield/plugin/plugin_host.hpp"
#include "shield/plugin/plugin_library.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace shield::plugin;

namespace {
struct MinimalTestInterface {
    static constexpr const char* interface_name = "minimal.test.iface";

    uint32_t struct_size;
    int marker;
};

std::string minimal_library_name() {
#ifdef SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY
    return SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY;
#else
    return "libshield_minimal_test_plugin.so";
#endif
}

const char* kMinimalManifest =
    R"(schema_version: 1
id: minimal.test
name: Minimal
version: 1.0.0
kind: test
entry: shield_plugin_get_v1
library:
  linux: bin/libshield_minimal_test_plugin.so
  macos: bin/libshield_minimal_test_plugin.dylib
  windows: bin/libshield_minimal_test_plugin.dll
provides:
  - interface: minimal.test.iface
requires: []
config_schema:
  type: object
  properties:
    start_fail:
      type: boolean
    register_lua_fail:
      type: boolean
    require_dependency:
      type: boolean
    wrong_interface_must_be_blocked:
      type: boolean
    port:
      type: integer
      minimum: 1
      maximum: 10
      default: 7
)";

const char* kDefaultConfigSchema =
    R"(config_schema:
  type: object
  properties:
    port:
      type: integer
      minimum: 1
      maximum: 10
      default: 7
    start_fail:
      type: boolean
    register_lua_fail:
      type: boolean
    require_dependency:
      type: boolean
    wrong_interface_must_be_blocked:
      type: boolean
)";

std::string make_manifest(std::string id,
                          std::string provides = "minimal.test.iface",
                          std::string requirements = "[]",
                          std::string config_schema = kDefaultConfigSchema) {
    std::string manifest =
        "schema_version: 1\n"
        "id: " + id + "\n"
        "name: " + id + "\n"
        "version: 1.0.0\n"
        "kind: test\n"
        "entry: shield_plugin_get_v1\n"
        "library:\n"
        "  linux: bin/" + minimal_library_name() + "\n"
        "  macos: bin/" + minimal_library_name() + "\n"
        "  windows: bin/" + minimal_library_name() + "\n";
    if (provides.empty()) {
        manifest += "provides: []\n";
    } else {
        manifest += "provides:\n  - interface: " + provides + "\n";
    }
    if (requirements == "[]") {
        manifest += "requires: []\n";
    } else {
        manifest += "requires:\n" + requirements;
        if (requirements.back() != '\n') manifest.push_back('\n');
    }
    manifest += config_schema;
    if (!manifest.empty() && manifest.back() != '\n') manifest.push_back('\n');
    return manifest;
}

fs::path unique_root(const std::string& name) {
    auto root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    return root;
}

// Writes a minimal package layout under a temp dir and returns the dir.
fs::path make_minimal_package() {
    fs::path root = unique_root("shield_plugin_host_test");
    fs::remove_all(root);
    fs::create_directories(root / "minimal.test" / "bin");
    std::ofstream(root / "minimal.test" / "manifest.yaml") << kMinimalManifest;
    return root;
}

fs::path make_runtime_package(const std::string& root_name = "shield_plugin_runtime_test") {
    fs::path root = unique_root(root_name);
    fs::create_directories(root / "minimal.test" / "bin");
    std::ofstream(root / "minimal.test" / "manifest.yaml")
        << make_manifest("minimal.test");
    fs::copy_file(fs::path(SHIELD_TEST_PLUGINS_DIR) / "minimal.test" / "bin" /
                      minimal_library_name(),
                  root / "minimal.test" / "bin" / minimal_library_name(),
                  fs::copy_options::overwrite_existing);
    return root;
}

struct MinimalPluginCounters {
    PluginLibrary lib;
    int (*started_count)() = nullptr;
    int (*shutdown_count)() = nullptr;
    void (*reset_counts)() = nullptr;
};

MinimalPluginCounters load_minimal_plugin_counters(const fs::path& library_path) {
    MinimalPluginCounters counters;
    std::string err;
    counters.lib = PluginLibrary::load(library_path.string(), err);
    BOOST_REQUIRE_MESSAGE(counters.lib.is_loaded(), err);
    counters.started_count = reinterpret_cast<int (*)()>(
        counters.lib.resolve("shield_minimal_test_started_count"));
    counters.shutdown_count = reinterpret_cast<int (*)()>(
        counters.lib.resolve("shield_minimal_test_shutdown_count"));
    counters.reset_counts = reinterpret_cast<void (*)()>(
        counters.lib.resolve("shield_minimal_test_reset_counts"));
    BOOST_REQUIRE(counters.started_count);
    BOOST_REQUIRE(counters.shutdown_count);
    BOOST_REQUIRE(counters.reset_counts);
    return counters;
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

BOOST_AUTO_TEST_CASE(scan_accepts_manifest_yaml_only_directory) {
    auto root = unique_root("shield_plugin_yaml_only_test");
    fs::create_directories(root / "yaml.only");
    std::ofstream(root / "yaml.only" / "manifest.yaml")
        << "schema_version: 1\n"
           "id: yaml.only\n"
           "entry: shield_plugin_get_v1\n"
           "library:\n"
           "  linux: bin/x.so\n"
           "  macos: bin/x.dylib\n"
           "  windows: bin/x.dll\n"
           "provides: []\n"
           "requires: []\n"
           "config_schema:\n"
           "  type: object\n";

    PluginHost host;
    host.scan(root.string());
    auto ids = host.package_ids();
    BOOST_REQUIRE_EQUAL(ids.size(), 1u);
    BOOST_CHECK_EQUAL(ids[0], "yaml.only");
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(scan_ignores_directories_without_manifest_yaml) {
    auto root = unique_root("shield_plugin_missing_manifest_test");
    fs::create_directories(root / "minimal.test" / "bin");
    std::ofstream(root / "minimal.test" / "README.md") << "no manifest";

    PluginHost host;
    host.scan(root.string());
    auto ids = host.package_ids();
    BOOST_CHECK(ids.empty());
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(catalog_rejects_duplicate_id) {
    auto root = make_minimal_package();
    fs::create_directories(root / "minimal.test.dup");
    std::ofstream(root / "minimal.test.dup" / "manifest.yaml") << kMinimalManifest;
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

BOOST_AUTO_TEST_CASE(optional_missing_package_becomes_unavailable) {
    auto root = make_minimal_package();
    PluginHost host;
    host.scan(root.string());
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    InstanceDecl in;
    in.id = "x";
    in.package = "does.not.exist";
    in.required = false;
    cfg.instances.push_back(in);
    BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
    const Instance* inst = host.find_instance("x");
    BOOST_REQUIRE(inst);
    BOOST_CHECK(inst->state == State::unavailable);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(binding_target_must_exist) {
    auto root = make_minimal_package();
    PluginHost host;
    host.scan(root.string());
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    BindingDecl b;
    b.logical = "minimal.default";
    b.instance_id = "missing";
    cfg.bindings.push_back(b);
    BOOST_CHECK(!host.plan_and_resolve(cfg, err));
    BOOST_TEST(err.find("plugin.binding.invalid") != std::string::npos);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(config_schema_is_enforced_during_resolve) {
    auto root = make_minimal_package();
    PluginHost host;
    host.scan(root.string());
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    InstanceDecl in;
    in.id = "m";
    in.package = "minimal.test";
    in.required = true;
    in.config = nlohmann::json{{"port", 99}};
    cfg.instances.push_back(in);
    BOOST_CHECK(!host.plan_and_resolve(cfg, err));
    BOOST_TEST(err.find("plugin.config.invalid") != std::string::npos);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(optional_bad_config_becomes_unavailable) {
    auto root = make_minimal_package();
    PluginHost host;
    host.scan(root.string());
    std::string err;
    PluginConfig cfg;
    cfg.directory = root.string();
    InstanceDecl in;
    in.id = "m";
    in.package = "minimal.test";
    in.required = false;
    in.config = nlohmann::json{{"port", 99}};
    cfg.instances.push_back(in);
    BOOST_REQUIRE(host.plan_and_resolve(cfg, err));
    const Instance* inst = host.find_instance("m");
    BOOST_REQUIRE(inst);
    BOOST_CHECK(inst->state == State::unavailable);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(resolve_detects_cycle) {
    // Two packages depending on each other.
    auto root = fs::temp_directory_path() / "shield_plugin_cycle_test";
    fs::remove_all(root);
    fs::create_directories(root / "a" / "bin");
    fs::create_directories(root / "b" / "bin");
    std::ofstream(root / "a" / "manifest.yaml")
        << make_manifest(
               "a",
               "iface.a",
               "  - name: b\n"
               "    interface: iface.b\n"
               "    optional: false\n",
               "config_schema:\n"
               "  type: object\n");
    std::ofstream(root / "b" / "manifest.yaml")
        << make_manifest(
               "b",
               "iface.b",
               "  - name: a\n"
               "    interface: iface.a\n"
               "    optional: false\n",
               "config_schema:\n"
               "  type: object\n");

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

BOOST_AUTO_TEST_CASE(load_create_start_pipeline_and_binding_access) {
    auto root = make_runtime_package();
    {
        PluginHost host;
        PluginConfig cfg;
        cfg.directory = root.string();
        InstanceDecl in;
        in.id = "m";
        in.package = "minimal.test";
        in.required = true;
        cfg.instances.push_back(in);
        BindingDecl b;
        b.logical = "minimal.default";
        b.instance_id = "m";
        cfg.bindings.push_back(b);

        std::string err;
        BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
        const Instance* inst = host.find_instance("m");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::started);
        auto* iface = host.get_by_binding<MinimalTestInterface>("minimal.default");
        BOOST_REQUIRE(iface != nullptr);
        BOOST_CHECK_EQUAL(iface->marker, 0x5A17);
        host.shutdown();
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(optional_missing_library_does_not_abort_startup) {
    auto root = unique_root("shield_plugin_missing_library_test");
    fs::create_directories(root / "minimal.test" / "bin");
    std::ofstream(root / "minimal.test" / "manifest.yaml")
        << make_manifest("minimal.test");

    PluginHost host;
    PluginConfig cfg;
    cfg.directory = root.string();
    InstanceDecl in;
    in.id = "m";
    in.package = "minimal.test";
    in.required = false;
    cfg.instances.push_back(in);

    std::string err;
    BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
    const Instance* inst = host.find_instance("m");
    BOOST_REQUIRE(inst);
    BOOST_CHECK(inst->state == State::unavailable);
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(optional_start_failure_becomes_unavailable) {
    auto root = make_runtime_package("shield_plugin_start_failure_test");
    {
        PluginHost host;
        PluginConfig cfg;
        cfg.directory = root.string();
        InstanceDecl in;
        in.id = "m";
        in.package = "minimal.test";
        in.required = false;
        in.config = nlohmann::json{{"start_fail", true}};
        cfg.instances.push_back(in);

        std::string err;
        BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
        const Instance* inst = host.find_instance("m");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::unavailable);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(required_start_failure_rolls_back_started_dependencies) {
    auto root = unique_root("shield_plugin_required_start_failure_test");
    fs::create_directories(root / "minimal.test" / "bin");
    std::ofstream(root / "minimal.test" / "manifest.yaml")
        << make_manifest("minimal.test",
                         "minimal.test.iface",
                         "  - name: dep\n"
                         "    interface: minimal.test.iface\n"
                         "    optional: true\n");
    fs::copy_file(fs::path(SHIELD_TEST_PLUGINS_DIR) / "minimal.test" / "bin" /
                      minimal_library_name(),
                  root / "minimal.test" / "bin" / minimal_library_name(),
                  fs::copy_options::overwrite_existing);

    {
        auto counters = load_minimal_plugin_counters(
            root / "minimal.test" / "bin" / minimal_library_name());
        counters.reset_counts();

        PluginHost host;
        PluginConfig cfg;
        cfg.directory = root.string();
        InstanceDecl dep;
        dep.id = "dep";
        dep.package = "minimal.test";
        cfg.instances.push_back(dep);
        InstanceDecl consumer;
        consumer.id = "consumer";
        consumer.package = "minimal.test";
        consumer.dependencies["dep"] = "dep";
        consumer.config = nlohmann::json{{"start_fail", true}};
        cfg.instances.push_back(consumer);

        std::string err;
        BOOST_CHECK(!host.startup(cfg, err));
        BOOST_TEST(err.find("plugin.init.failed") != std::string::npos);
        BOOST_CHECK_EQUAL(counters.started_count(), 1);
        BOOST_CHECK_EQUAL(counters.shutdown_count(), 2);
        const Instance* dep_inst = host.find_instance("dep");
        BOOST_REQUIRE(dep_inst);
        BOOST_CHECK(dep_inst->state == State::stopped);
        const Instance* consumer_inst = host.find_instance("consumer");
        BOOST_REQUIRE(consumer_inst);
        BOOST_CHECK(consumer_inst->state == State::failed);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(declared_dependency_is_injected_and_wrong_interface_is_blocked) {
    auto root = unique_root("shield_plugin_dependency_test");
    fs::create_directories(root / "minimal.test" / "bin");
    std::ofstream(root / "minimal.test" / "manifest.yaml")
        << make_manifest("minimal.test",
                         "minimal.test.iface",
                         "  - name: dep\n"
                         "    interface: minimal.test.iface\n"
                         "    optional: true\n");
    fs::copy_file(fs::path(SHIELD_TEST_PLUGINS_DIR) / "minimal.test" / "bin" /
                      minimal_library_name(),
                  root / "minimal.test" / "bin" / minimal_library_name(),
                  fs::copy_options::overwrite_existing);

    {
        PluginHost host;
        PluginConfig cfg;
        cfg.directory = root.string();
        InstanceDecl dep;
        dep.id = "dep";
        dep.package = "minimal.test";
        cfg.instances.push_back(dep);
        InstanceDecl consumer;
        consumer.id = "consumer";
        consumer.package = "minimal.test";
        consumer.dependencies["dep"] = "dep";
        consumer.config = nlohmann::json{{"require_dependency", true},
                                         {"wrong_interface_must_be_blocked", true}};
        cfg.instances.push_back(consumer);

        std::string err;
        BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
        BOOST_CHECK(host.find_instance("consumer")->state == State::started);
        host.shutdown();
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(required_register_lua_failure_is_reported_without_stopping_instance) {
    auto root = make_runtime_package("shield_plugin_lua_failure_required_test");
    {
        PluginHost host;
        PluginConfig cfg;
        cfg.directory = root.string();
        InstanceDecl in;
        in.id = "m";
        in.package = "minimal.test";
        in.required = true;
        in.config = nlohmann::json{{"register_lua_fail", true}};
        cfg.instances.push_back(in);

        std::string err;
        BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
        BOOST_REQUIRE(host.find_instance("m"));
        BOOST_CHECK(host.find_instance("m")->state == State::started);

        err.clear();
        BOOST_CHECK(!host.register_lua_all(reinterpret_cast<lua_State*>(0x1), err));
        BOOST_TEST(err.find("plugin.lua_register.failed") != std::string::npos);
        BOOST_REQUIRE(host.find_instance("m"));
        BOOST_CHECK(host.find_instance("m")->state == State::started);
        host.shutdown();
        BOOST_REQUIRE(host.find_instance("m"));
        BOOST_CHECK(host.find_instance("m")->state == State::stopped);
    }
    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(optional_register_lua_failure_is_recorded_and_skipped) {
    auto root = make_runtime_package("shield_plugin_lua_failure_optional_test");
    {
        PluginHost host;
        PluginConfig cfg;
        cfg.directory = root.string();
        InstanceDecl in;
        in.id = "m";
        in.package = "minimal.test";
        in.required = false;
        in.config = nlohmann::json{{"register_lua_fail", true}};
        cfg.instances.push_back(in);

        std::string err;
        BOOST_REQUIRE_MESSAGE(host.startup(cfg, err), err);
        BOOST_CHECK(host.register_lua_all(reinterpret_cast<lua_State*>(0x1), err));
        const Instance* inst = host.find_instance("m");
        BOOST_REQUIRE(inst);
        BOOST_CHECK(inst->state == State::started);
        BOOST_TEST(inst->last_error.find("plugin.lua_register.failed") != std::string::npos);
        host.shutdown();
    }
    fs::remove_all(root);
}
