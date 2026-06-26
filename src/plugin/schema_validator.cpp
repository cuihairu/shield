// [SHIELD_PLUGIN] Minimal JSON-Schema subset validator implementation.
#include "schema_validator.hpp"

#include <vector>

namespace shield::plugin {

namespace {
bool check_type(const std::string& want, const nlohmann::json& v) {
    if (want == "object") return v.is_object();
    if (want == "array") return v.is_array();
    if (want == "string") return v.is_string();
    if (want == "integer") return v.is_number_integer();
    if (want == "number") return v.is_number();
    if (want == "boolean") return v.is_boolean();
    return true;  // unknown types: lenient (forward-compat)
}

std::string join_path(const std::string& base, const std::string& key) {
    return base.empty() ? key : (base + "." + key);
}
}  // namespace

std::string validate_config(const nlohmann::json& schema,
                            const nlohmann::json& value,
                            const std::string& path) {
    if (schema.contains("type")) {
        const auto t = schema.at("type").get<std::string>();
        if (!check_type(t, value)) {
            return path.empty() ? ("type mismatch: expected " + t)
                                : (path + ": type mismatch, expected " + t);
        }
    }

    if (value.is_object()) {
        if (schema.contains("required") && schema.at("required").is_array()) {
            for (const auto& k : schema.at("required")) {
                const auto key = k.get<std::string>();
                if (!value.contains(key)) {
                    return join_path(path, key) + ": required field missing";
                }
            }
        }
        if (schema.contains("properties") &&
            schema.at("properties").is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (schema.at("properties").contains(it.key())) {
                    auto err =
                        validate_config(schema.at("properties").at(it.key()),
                                        it.value(), join_path(path, it.key()));
                    if (!err.empty()) return err;
                }
            }
        }
    }

    if (value.is_number()) {
        if (schema.contains("minimum") &&
            value.get<double>() < schema.at("minimum").get<double>()) {
            return path + ": below minimum";
        }
        if (schema.contains("maximum") &&
            value.get<double>() > schema.at("maximum").get<double>()) {
            return path + ": above maximum";
        }
    }

    if (schema.contains("enum") && schema.at("enum").is_array()) {
        bool ok = false;
        for (const auto& e : schema.at("enum")) {
            if (e == value) {
                ok = true;
                break;
            }
        }
        if (!ok) return path + ": value not in enum";
    }

    if (value.is_array() && schema.contains("items")) {
        size_t i = 0;
        for (const auto& el : value) {
            auto err = validate_config(schema.at("items"), el,
                                       path + "[" + std::to_string(i++) + "]");
            if (!err.empty()) return err;
        }
    }

    return {};
}

void apply_defaults(const nlohmann::json& schema, nlohmann::json& value) {
    if (!value.is_object() || !schema.contains("properties") ||
        !schema.at("properties").is_object()) {
        return;
    }
    for (auto it = schema.at("properties").begin();
         it != schema.at("properties").end(); ++it) {
        const auto& key = it.key();
        const auto& sub = it.value();
        if (!value.contains(key) && sub.contains("default")) {
            value[key] = sub.at("default");
        } else if (value.contains(key) && value[key].is_object()) {
            apply_defaults(sub, value[key]);
        }
    }
}

void collect_secret_paths(const nlohmann::json& schema, const std::string& path,
                          std::vector<std::string>& out) {
    if (!schema.is_object()) return;
    if (schema.value("secret", false)) {
        out.push_back(path);
    }
    if (schema.contains("properties") && schema.at("properties").is_object()) {
        for (auto it = schema.at("properties").begin();
             it != schema.at("properties").end(); ++it) {
            collect_secret_paths(it.value(), join_path(path, it.key()), out);
        }
    }
}

}  // namespace shield::plugin
