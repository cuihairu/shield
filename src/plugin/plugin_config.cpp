// [SHIELD_PLUGIN] PluginConfig parsing from the app.yaml `plugins:` subtree.
//
// The main config stays YAML (Config subsystem unchanged). We read the
// `plugins` subtree via Config::to_json() (YAML→JSON), then parse
// instances/bindings with nlohmann. Instance `config` values are JSON
// structures (YAML is a JSON superset, so the on-disk YAML is compatible).
#include "shield/plugin/plugin_host.hpp"
#include "shield/config/config.hpp"

namespace shield::plugin {

namespace {
InstanceDecl parse_instance(const nlohmann::json& in) {
    InstanceDecl d;
    d.id = in.value("id", std::string());
    d.package = in.value("package", std::string());
    d.required = in.value("required", true);
    if (in.contains("dependencies") && in["dependencies"].is_object()) {
        for (auto it = in.at("dependencies").begin();
             it != in.at("dependencies").end(); ++it) {
            d.dependencies[it.key()] = it.value().get<std::string>();
        }
    }
    d.config = in.value("config", nlohmann::json::object());
    return d;
}
}  // namespace

PluginConfig parse_plugin_config(const shield::config::Config& cfg) {
    PluginConfig pc;
    pc.directory = cfg.get_string("plugins.directory", "./plugins");

    // Config does not expose a subtree walker, so round-trip through to_json()
    // and parse the `plugins` node with nlohmann.
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(cfg.to_json());
    } catch (...) {
        return pc;  // unparseable global config → empty plugin config
    }
    if (!root.contains("plugins")) return pc;
    const auto& plugins = root["plugins"];

    if (plugins.contains("instances") && plugins["instances"].is_array()) {
        for (const auto& in : plugins.at("instances")) {
            pc.instances.push_back(parse_instance(in));
        }
    }
    if (plugins.contains("bindings") && plugins["bindings"].is_object()) {
        for (auto it = plugins.at("bindings").begin();
             it != plugins.at("bindings").end(); ++it) {
            BindingDecl b;
            b.logical = it.key();
            b.instance_id = it.value().get<std::string>();
            pc.bindings.push_back(std::move(b));
        }
    }
    return pc;
}

PluginConfig load_plugin_config() {
    return parse_plugin_config(shield::config::global_config());
}

}  // namespace shield::plugin
