// Shared payload-schema settings for schema-less protocol codec plugins
// (protocol.json, protocol.msgpack). Schemas are opt-in per route and keyed
// by the host-resolved schema type name (RouteEntry.schema_name), mirroring
// the codec ABI's single-level "name -> type" lookup. Validation reuses the
// host's minimal JSON-Schema subset validator (src/plugin/schema_validator).
#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "schema_validator.hpp"

namespace shield::plugins {

struct payload_schema_settings {
    nlohmann::json schemas;  // object: <schema_type_name> -> subset schema
    bool require_schema = false;
    std::string on_violation = "reject";  // reject | warn
};

// Parse the shared config keys ("schemas" | "schemas_file", "require_schema",
// "on_violation") from an instance config object. Returns false with a
// message in *error on any shape violation; callers surface it at create
// time. The host's manifest config_schema checks the key types too — these
// guards are defense in depth for direct-call tests and hand-built configs.
inline bool load_payload_schema_settings(payload_schema_settings& out,
                                         const nlohmann::json& config,
                                         std::string* error) {
    out = payload_schema_settings{};
    if (!config.is_object()) {
        *error = "config must be a JSON object";
        return false;
    }
    const bool has_inline = config.contains("schemas");
    const bool has_file = config.contains("schemas_file");
    if (has_inline && has_file) {
        *error = "schemas and schemas_file are mutually exclusive";
        return false;
    }
    if (has_inline) {
        if (!config.at("schemas").is_object()) {
            *error = "schemas must be a JSON object";
            return false;
        }
        for (auto it = config.at("schemas").begin();
             it != config.at("schemas").end(); ++it) {
            if (!it.value().is_object()) {
                *error = "schemas." + it.key() + ": schema must be an object";
                return false;
            }
        }
        out.schemas = config.at("schemas");
    }
    if (has_file) {
        if (!config.at("schemas_file").is_string()) {
            *error = "schemas_file must be a string path";
            return false;
        }
        const auto path = config.at("schemas_file").get<std::string>();
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            *error = "schemas_file: cannot open " + path;
            return false;
        }
        std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        auto parsed = nlohmann::json::parse(data, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            *error = "schemas_file: not a JSON object: " + path;
            return false;
        }
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            if (!it.value().is_object()) {
                *error = "schemas_file " + path + ": " + it.key() +
                         ": schema must be an object";
                return false;
            }
        }
        out.schemas = std::move(parsed);
    }
    if (config.contains("require_schema")) {
        if (!config.at("require_schema").is_boolean()) {
            *error = "require_schema must be a boolean";
            return false;
        }
        out.require_schema = config.at("require_schema").get<bool>();
    }
    if (config.contains("on_violation")) {
        if (!config.at("on_violation").is_string() ||
            (config.at("on_violation").get<std::string>() != "reject" &&
             config.at("on_violation").get<std::string>() != "warn")) {
            *error = "on_violation must be \"reject\" or \"warn\"";
            return false;
        }
        out.on_violation = config.at("on_violation").get<std::string>();
    }
    return true;
}

// Single-level lookup by the ABI's only addressing key. A null route_name
// counts as "no schema".
inline const nlohmann::json* find_payload_schema(
    const payload_schema_settings& settings, const char* route_name) {
    if (route_name == nullptr) return nullptr;
    const auto it = settings.schemas.find(route_name);
    return it == settings.schemas.end() ? nullptr : &it.value();
}

// Validate a message against a schema; returns false with a diagnostic in
// *violation. Schema evaluation errors (malformed keyword types) are caught
// here so they surface as a decode/encode failure instead of escaping the
// plugin's C ABI boundary.
inline bool validate_payload_schema(const nlohmann::json& schema,
                                    const nlohmann::json& message,
                                    std::string* violation) {
    try {
        *violation = shield::plugin::validate_config(schema, message);
        return violation->empty();
    } catch (const std::exception& e) {
        *violation = std::string("schema evaluation error: ") + e.what();
        return false;
    }
}

}  // namespace shield::plugins
