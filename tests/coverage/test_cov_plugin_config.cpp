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
