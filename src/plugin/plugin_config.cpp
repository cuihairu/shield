// [SHIELD_PLUGIN] PluginConfig parsing from the app.yaml `plugins:` subtree.
//
// The main config stays YAML (Config subsystem unchanged). We read the
// `plugins` subtree via Config::to_json() (YAML→JSON), then parse
// instances/bindings with nlohmann. Instance `config` values are JSON
// structures (YAML is a JSON superset, so the on-disk YAML is compatible).
#include "shield/config/config.hpp"
#include "shield/plugin/plugin_host.hpp"

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

PluginConfig parse_plugin_config_json(std::string_view json_text) {
    PluginConfig pc;

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_text);
    } catch (...) {
        return pc;  // unparseable global config → empty plugin config
    }
    if (!root.contains("plugins")) return pc;
    const auto& plugins = root["plugins"];

    if (plugins.contains("directory") && plugins["directory"].is_string()) {
        pc.directory = plugins["directory"].get<std::string>();
    }
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

PluginConfig parse_plugin_config(const shield::config::Config& cfg) {
    // The flat Config::to_json() view only carries dotted scalar keys, so
    // nested structures (instances list, bindings map) are invisible there.
    // Serialize the full `plugins` subtree from the YAML root instead and
    // feed it to the JSON parser wrapped under its expected "plugins" key.
    PluginConfig pc;
    // The flat "plugins.directory" key never nests in to_json() output, so
    // read it directly from the Config view.
    pc.directory = cfg.get_string("plugins.directory", "./plugins");
    PluginConfig rest;
    try {
        rest = parse_plugin_config_json(
            "{\"plugins\":" + shield::config::subtree_json(cfg, "plugins") +
            "}");
    } catch (...) {
        return pc;  // unparseable global config → directory-only result
    }
    pc.instances = std::move(rest.instances);
    pc.bindings = std::move(rest.bindings);
    return pc;
}

PluginConfig load_plugin_config() {
    return parse_plugin_config(shield::config::global_config());
}

}  // namespace shield::plugin
