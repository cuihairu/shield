// [SHIELD_PLUGIN] Minimal JSON-Schema subset validator.
//
// Validates an instance config against the config_schema declared in a
// plugin manifest. Supports the small keyword set documented in
// docs/plugin-system.md §Config Schema:
//   type, required, properties, default, minimum, maximum, enum, items,
//   secret (recognized for masking, not enforced here).
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace shield::plugin {

// Validate `value` against `schema`. Returns empty string on success, or a
// human-readable error path (e.g. "port: below minimum") on failure.
std::string validate_config(const nlohmann::json& schema,
                            const nlohmann::json& value,
                            const std::string& path = "");

// Apply schema defaults into `value` (mutates in place). Recurses into
// nested objects.
void apply_defaults(const nlohmann::json& schema, nlohmann::json& value);

// Collect the set of "secret": true leaf paths (dot-paths) for masking.
// Returns them in `out`.
void collect_secret_paths(const nlohmann::json& schema, const std::string& path,
                          std::vector<std::string>& out);

}  // namespace shield::plugin
