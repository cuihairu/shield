// [SHIELD_PLUGIN] Manifest parsing + validation.
#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "shield/plugin/plugin_host.hpp"

namespace shield::plugin {

namespace {
// Throw if `j` lacks `key` or it is null.
void require_field(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) {
        throw std::runtime_error(
            std::string("plugin.manifest.invalid: missing '") + key + "'");
    }
}

nlohmann::json yaml_to_json(const YAML::Node& node) {
    if (!node || node.IsNull()) {
        return nullptr;
    }
    if (node.IsScalar()) {
        try {
            return node.as<bool>();
        } catch (const std::exception&) {
        }
        try {
            return node.as<std::int64_t>();
        } catch (const std::exception&) {
        }
        try {
            return node.as<double>();
        } catch (const std::exception&) {
        }
        return node.as<std::string>();
    }
    if (node.IsSequence()) {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& item : node) {
            array.push_back(yaml_to_json(item));
        }
        return array;
    }
    if (node.IsMap()) {
        nlohmann::json object = nlohmann::json::object();
        for (const auto& item : node) {
            object[item.first.as<std::string>()] = yaml_to_json(item.second);
        }
        return object;
    }
    return nullptr;
}
}  // namespace

Manifest parse_manifest(const nlohmann::json& j) {
    Manifest m;

    require_field(j, "schema_version");
    m.schema_version = j.at("schema_version").get<int>();
    if (m.schema_version != 1) {
        throw std::runtime_error(
            "plugin.manifest.invalid: schema_version must be 1");
    }

    require_field(j, "id");
    m.id = j.at("id").get<std::string>();
    require_field(j, "entry");
    m.entry = j.at("entry").get<std::string>();

    m.name = j.value("name", m.id);
    m.version = j.value("version", std::string("0.0.0"));
    m.kind = j.value("kind", std::string());
    m.description = j.value("description", std::string());

    require_field(j, "library");
    const auto& lib = j.at("library");
    m.library.linux = lib.value("linux", std::string());
    m.library.macos = lib.value("macos", std::string());
    m.library.windows = lib.value("windows", std::string());

    require_field(j, "provides");
    for (const auto& p : j.at("provides")) {
        Manifest::Provide pr;
        require_field(p, "interface");
        pr.interface_name = p.at("interface").get<std::string>();
        if (p.contains("capabilities") && p["capabilities"].is_array()) {
            pr.capabilities =
                p.at("capabilities").get<std::vector<std::string>>();
        }
        m.provides.push_back(std::move(pr));
    }

    if (j.contains("requires") && j["requires"].is_array()) {
        for (const auto& r : j.at("requires")) {
            Manifest::Require rq;
            require_field(r, "name");
            rq.name = r.at("name").get<std::string>();
            require_field(r, "interface");
            rq.interface_name = r.at("interface").get<std::string>();
            rq.optional = r.value("optional", false);
            m.requires_.push_back(std::move(rq));
        }
    }

    // Optional Lua metadata. When present, the host injects the declared
    // search_paths into Lua's package.path before register_lua_all runs.
    if (j.contains("lua") && j["lua"].is_object()) {
        const auto& l = j.at("lua");
        if (l.contains("namespace") && !l["namespace"].is_null()) {
            m.lua.namespace_ = l.at("namespace").get<std::string>();
        }
        if (l.contains("search_paths") && l["search_paths"].is_array()) {
            for (const auto& p : l.at("search_paths")) {
                if (p.is_string())
                    m.lua.search_paths.push_back(p.get<std::string>());
            }
        }
        m.lua.enabled =
            !m.lua.namespace_.empty() || !m.lua.search_paths.empty();
    }

    // Optional documentation pointer. Surfaced via list_packages() so
    // dashboards / introspection APIs can deep-link to per-plugin docs.
    if (j.contains("documentation") && j["documentation"].is_object()) {
        const auto& d = j.at("documentation");
        if (d.contains("url") && d["url"].is_string()) {
            m.documentation.url = d.at("url").get<std::string>();
        }
        if (d.contains("description") && d["description"].is_string()) {
            m.documentation.description =
                d.at("description").get<std::string>();
        }
        m.documentation.enabled = !m.documentation.url.empty();
    }

    m.config_schema = j.value("config_schema", nlohmann::json::object());
    return m;
}

Manifest load_manifest_file(const std::filesystem::path& manifest_path) {
    if (manifest_path.filename() != "manifest.yaml") {
        throw std::runtime_error(
            "plugin.manifest.invalid: manifest file must be named "
            "manifest.yaml");
    }
    std::ifstream f(manifest_path);
    if (!f) {
        throw std::runtime_error("plugin.manifest.invalid: cannot open " +
                                 manifest_path.string());
    }
    std::stringstream ss;
    ss << f.rdbuf();

    try {
        return parse_manifest(yaml_to_json(YAML::Load(ss.str())));
    } catch (const YAML::ParserException& e) {
        throw std::runtime_error(
            std::string("plugin.manifest.invalid: YAML parse error in ") +
            manifest_path.string() + ": " + e.what());
    } catch (const YAML::BadConversion& e) {
        throw std::runtime_error(
            std::string("plugin.manifest.invalid: YAML conversion error in ") +
            manifest_path.string() + ": " + e.what());
    }
}

std::string platform_library_path(const Manifest& m) {
#if defined(_WIN32)
    return m.library.windows;
#elif defined(__APPLE__)
    return m.library.macos;
#else
    return m.library.linux;
#endif
}

}  // namespace shield::plugin
