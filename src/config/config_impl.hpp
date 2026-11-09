// [SHIELD_CONFIG] Internal cross-TU declarations for the config split.
//
// Not a public contract: the public API stays in
// shield/config/config.hpp. StorageValue and Config::Impl move here
// verbatim so the config*.cpp translation units share one storage
// definition; the two file-local helpers shared across the split are
// defined flat (external linkage) in their own TU, one declaration
// each.
#pragma once

#include <yaml-cpp/yaml.h>

#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "shield/config/config.hpp"

namespace shield::config {

// Internal storage type
using StorageValue = std::variant<std::string, int64_t, double, bool,
                                  std::vector<std::string>, YAML::Node>;

struct Config::Impl {
    std::unordered_map<std::string, StorageValue> storage;
    mutable std::shared_mutex mutex;
    YAML::Node root;  // Keep original YAML for complex queries
    std::string source_dir;
};

// Defined in config_yaml.cpp: YAML-to-JSON conversion shared by the
// runtime accessors and subtree_json.
nlohmann::json yaml_to_json(const YAML::Node& node);

// Defined in config_validate.cpp: scalar extractor shared by the
// validators and the runtime accessors.
std::optional<int> scalar_int(const YAML::Node& node, const char* key);

}  // namespace shield::config
