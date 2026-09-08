#define BOOST_TEST_MODULE CovPluginConfig
#include <boost/test/unit_test.hpp>

#include "shield/config/config.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace {

// Full plugins subtree as it would appear in app.yaml. NOTE: Config stores
// flattened dotted keys ("plugins.directory", ...) and to_json() emits those
// flat keys verbatim, so the nested "plugins" JSON object parse_plugin_config
// looks for never materializes from YAML-loaded data.
const char* kFullPluginsYaml =
    "plugins:\n"
    "  directory: /opt/shield/plugins\n"
    "  instances:\n"
    "    - id: db.main\n"
    "      package: database.sqlite\n"
    "      required: false\n"
    "      dependencies:\n"
    "        db: db.main\n"
    "      config:\n"
    "        port: 3306\n"
    "  bindings:\n"
    "    database.default: db.main\n";

void check_empty(const shield::plugin::PluginConfig& pc) {
    BOOST_CHECK(pc.instances.empty());
    BOOST_CHECK(pc.bindings.empty());
}

}  // namespace

// Without a plugins: subtree everything stays at defaults (early return on
// the missing root node).
BOOST_AUTO_TEST_CASE(config_without_plugins_returns_defaults) {
    shield::config::Config cfg;
    BOOST_REQUIRE(cfg.load_yaml_string("server:\n  port: 9000\n"));
    auto pc = shield::plugin::parse_plugin_config(cfg);
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    check_empty(pc);
}

// A YAML plugins: subtree flattens to dotted keys, so the parsed JSON root
// has no "plugins" member and parse_plugin_config short-circuits; the
// directory is still read via get_string("plugins.directory").
BOOST_AUTO_TEST_CASE(yaml_plugins_subtree_flattens_to_default_result) {
    shield::config::Config cfg;
    BOOST_REQUIRE(cfg.load_yaml_string(kFullPluginsYaml));
    auto pc = shield::plugin::parse_plugin_config(cfg);
    BOOST_CHECK_EQUAL(pc.directory, "/opt/shield/plugins");
    check_empty(pc);
}

// A literal top-level "plugins" storage entry survives to_json() as a scalar
// (Config cannot hold objects), so the instances/bindings branches are
// evaluated but skipped.
BOOST_AUTO_TEST_CASE(scalar_plugins_key_skips_instances_and_bindings) {
    shield::config::Config cfg;
    cfg.set("plugins", std::string("not-an-object"));
    auto pc = shield::plugin::parse_plugin_config(cfg);
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    check_empty(pc);
}

// to_json() throws on invalid UTF-8 values (nlohmann dump rejects them), so
// json::parse never sees input and the catch-all returns an empty config.
BOOST_AUTO_TEST_CASE(unparseable_json_output_yields_empty_config) {
    shield::config::Config cfg;
    cfg.set("bad", std::string("x\xff\xfe"));
    auto pc = shield::plugin::parse_plugin_config(cfg);
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    check_empty(pc);
}

// load_plugin_config() reads the process-wide global config snapshot.
BOOST_AUTO_TEST_CASE(load_plugin_config_uses_global_config) {
    shield::config::reset_config();
    shield::config::global_config().load_yaml_string(
        "plugins:\n  directory: /global/plugins\n");
    auto pc = shield::plugin::load_plugin_config();
    BOOST_CHECK_EQUAL(pc.directory, "/global/plugins");
    check_empty(pc);
    shield::config::reset_config();
}

// ---------------------------------------------------------------------------
// parse_plugin_config_json: nested plugins object parsing (the Config view
// flattens keys, so these branches are exercised via the JSON seam).
// ---------------------------------------------------------------------------

// Full nested document: directory + instances (with dependencies/config) +
// bindings all populate the PluginConfig.
BOOST_AUTO_TEST_CASE(json_full_plugins_document_parses_all_fields) {
    const char* kJson = R"({
        "plugins": {
            "directory": "/opt/shield/plugins",
            "instances": [
                {
                    "id": "db.main",
                    "package": "database.sqlite",
                    "required": false,
                    "dependencies": {"db": "db.main", "cache": "redis.main"},
                    "config": {"port": 3306, "host": "127.0.0.1"}
                }
            ],
            "bindings": {
                "database.default": "db.main",
                "cache.default": "redis.main"
            }
        }
    })";
    auto pc = shield::plugin::parse_plugin_config_json(kJson);
    BOOST_CHECK_EQUAL(pc.directory, "/opt/shield/plugins");
    BOOST_REQUIRE_EQUAL(pc.instances.size(), 1u);
    const auto& inst = pc.instances.front();
    BOOST_CHECK_EQUAL(inst.id, "db.main");
    BOOST_CHECK_EQUAL(inst.package, "database.sqlite");
    BOOST_CHECK(!inst.required);
    BOOST_REQUIRE_EQUAL(inst.dependencies.size(), 2u);
    BOOST_CHECK_EQUAL(inst.dependencies.at("db"), "db.main");
    BOOST_CHECK_EQUAL(inst.dependencies.at("cache"), "redis.main");
    BOOST_CHECK_EQUAL(inst.config.at("port").get<int>(), 3306);
    BOOST_CHECK_EQUAL(inst.config.at("host").get<std::string>(), "127.0.0.1");
    BOOST_REQUIRE_EQUAL(pc.bindings.size(), 2u);
    BOOST_CHECK_EQUAL(pc.bindings[0].logical, "cache.default");
    BOOST_CHECK_EQUAL(pc.bindings[0].instance_id, "redis.main");
    BOOST_CHECK_EQUAL(pc.bindings[1].logical, "database.default");
    BOOST_CHECK_EQUAL(pc.bindings[1].instance_id, "db.main");
}

// An instance with no fields falls back to defaults (required=true, empty
// id/package/dependencies, empty JSON object config).
BOOST_AUTO_TEST_CASE(json_instance_defaults_when_fields_missing) {
    auto pc = shield::plugin::parse_plugin_config_json(
        R"({"plugins": {"instances": [{}]}})");
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    BOOST_REQUIRE_EQUAL(pc.instances.size(), 1u);
    const auto& inst = pc.instances.front();
    BOOST_CHECK(inst.id.empty());
    BOOST_CHECK(inst.package.empty());
    BOOST_CHECK(inst.required);
    BOOST_CHECK(inst.dependencies.empty());
    BOOST_CHECK(inst.config.is_object());
    BOOST_CHECK(inst.config.empty());
}

// A non-object "dependencies" value is ignored by parse_instance.
BOOST_AUTO_TEST_CASE(json_instance_non_object_dependencies_skipped) {
    auto pc = shield::plugin::parse_plugin_config_json(
        R"({"plugins": {"instances": [
            {"id": "a", "dependencies": "not-an-object"}
        ]}})");
    BOOST_REQUIRE_EQUAL(pc.instances.size(), 1u);
    BOOST_CHECK_EQUAL(pc.instances.front().id, "a");
    BOOST_CHECK(pc.instances.front().dependencies.empty());
}

// Non-array "instances" and non-object "bindings" values are skipped; a
// non-string "directory" keeps the default.
BOOST_AUTO_TEST_CASE(json_mistyped_members_are_skipped) {
    auto pc = shield::plugin::parse_plugin_config_json(
        R"({"plugins": {
            "directory": 42,
            "instances": {"not": "an-array"},
            "bindings": ["not", "an", "object"]
        }})");
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    check_empty(pc);
}

// Invalid JSON yields a default PluginConfig (catch-all path).
BOOST_AUTO_TEST_CASE(json_invalid_input_yields_default_config) {
    auto pc = shield::plugin::parse_plugin_config_json("{not json");
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    check_empty(pc);
}

// A document without a "plugins" member short-circuits to defaults.
BOOST_AUTO_TEST_CASE(json_without_plugins_member_yields_default_config) {
    auto pc = shield::plugin::parse_plugin_config_json(R"({"server": {}})");
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    check_empty(pc);
}

// Multiple instances are all parsed in order.
BOOST_AUTO_TEST_CASE(json_multiple_instances_preserve_order) {
    auto pc = shield::plugin::parse_plugin_config_json(
        R"({"plugins": {"instances": [
            {"id": "first", "package": "p1"},
            {"id": "second", "package": "p2", "required": false}
        ]}})");
    BOOST_REQUIRE_EQUAL(pc.instances.size(), 2u);
    BOOST_CHECK_EQUAL(pc.instances[0].id, "first");
    BOOST_CHECK_EQUAL(pc.instances[1].id, "second");
    BOOST_CHECK(!pc.instances[1].required);
}
