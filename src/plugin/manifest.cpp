// [SHIELD_PLUGIN] Manifest (plugin.json) parsing + validation.
#include "shield/plugin/plugin_host.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace shield::plugin {

namespace {
// Throw if `j` lacks `key` or it is null.
void require_field(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) {
        throw std::runtime_error(
            std::string("plugin.manifest.invalid: missing '") + key + "'");
    }
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
                if (p.is_string()) m.lua.search_paths.push_back(p.get<std::string>());
            }
        }
        m.lua.enabled = !m.lua.namespace_.empty() || !m.lua.search_paths.empty();
    }

    // Optional documentation pointer. Surfaced via list_packages() so
    // dashboards / introspection APIs can deep-link to per-plugin docs.
    if (j.contains("documentation") && j["documentation"].is_object()) {
        const auto& d = j.at("documentation");
        if (d.contains("url") && d["url"].is_string()) {
            m.documentation.url = d.at("url").get<std::string>();
        }
        if (d.contains("description") && d["description"].is_string()) {
            m.documentation.description = d.at("description").get<std::string>();
        }
        m.documentation.enabled = !m.documentation.url.empty();
    }

    m.config_schema = j.value("config_schema", nlohmann::json::object());
    return m;
}

Manifest load_manifest_file(const std::filesystem::path& plugin_json) {
    std::ifstream f(plugin_json);
    if (!f) {
        throw std::runtime_error("plugin.manifest.invalid: cannot open " +
                                 plugin_json.string());
    }
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        return parse_manifest(nlohmann::json::parse(ss.str()));
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(
            std::string("plugin.manifest.invalid: JSON parse error in ") +
            plugin_json.string() + ": " + e.what());
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
