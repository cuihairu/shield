// [SHIELD_CONFIG] YAML loading: node merge/flatten helpers, the
// YAML-to-JSON converter shared with the runtime accessors and
// Config::load_yaml / load_yaml_string.
// Split out of config.cpp.
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "config_impl.hpp"
#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/net/ip_blocklist.hpp"

namespace shield::log {
class Logger;
}

namespace shield::config {

namespace {
YAML::Node merge_yaml_nodes(const YAML::Node& base, const YAML::Node& overlay) {
    if (!base || base.IsNull()) {
        return YAML::Clone(overlay);
    }
    if (!overlay ||  // GCOVR_EXCL_BR_LINE (defensive: overlay always defined)
        overlay.IsNull()) {  // GCOVR_EXCL_BR_LINE (defensive: the overlay
                             // always comes from a loaded YAML document root or
                             // a mapping child, so it is defined; null overlays
                             // take the covered IsNull arm)
        return YAML::Clone(base);
    }
    if (!base.IsMap() || !overlay.IsMap()) {
        return YAML::Clone(overlay);
    }

    YAML::Node merged = YAML::Clone(base);
    for (const auto& entry : overlay) {
        const std::string key = entry.first.as<std::string>();
        merged[key] = merge_yaml_nodes(merged[key], entry.second);
    }
    return merged;
}

void flatten_yaml_node(const YAML::Node& root,
                       std::unordered_map<std::string, StorageValue>& storage) {
    storage.clear();

    std::function<void(const YAML::Node&, const std::string&)> flatten;
    flatten = [&](const YAML::Node& node, const std::string& prefix) {
        if (!node.IsMap()) {
            return;
        }

        for (auto it = node.begin(); it != node.end(); ++it) {
            // Dereference into a named object: binding `it->second` to a
            // reference goes through yaml-cpp's operator-> proxy, and the
            // lifetime extension of that temporary trips gcc's ASan
            // stack-use-after-scope instrumentation. A named copy is
            // unambiguous for compilers and sanitizers alike.
            const auto entry = *it;
            std::string key = entry.first.as<std::string>();
            std::string full_key =
                prefix.empty()
                    ? key
                    : prefix + "." +  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // never-executed gcov clone arms; both
                                      // ternary arms are covered in tests)
                          key;  // GCOVR_EXCL_BR_LINE (compiler artifact: both
                                // arms of the extra branch are never-executed
                                // gcov clones)
            const YAML::Node& value = entry.second;

            if (value.IsScalar()) {
                // yaml-cpp reports explicitly tagged scalars with the
                // canonical URI form ("tag:yaml.org,2002:int"), not the
                // shorthand ("!!int"); accept both so tagged scalars are
                // stored with their typed variant.
                const std::string tag = value.Tag();
                const auto is_tag = [&tag](const char* shorthand) {
                    return tag == shorthand ||  // GCOVR_EXCL_BR_LINE
                                                // (defensive: yaml-cpp 0.9
                                                // always reports canonical tag
                                                // URIs, so the shorthand
                                                // equality arm can never match)
                           tag ==
                               "tag:yaml.org,2002:" +  // GCOVR_EXCL_BR_LINE
                                                       // (compiler artifact:
                                                       // never-executed gcov
                                                       // clone arms)
                                   std::string(  // GCOVR_EXCL_BR_LINE (compiler
                                                 // artifact: inlined
                                                 // std::string ctor arms plus
                                                 // gcov clone arms)
                                       shorthand +
                                       2);  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: inlined std::string
                                            // construction branches plus
                                            // never-executed gcov clone arms)
                };
                if (is_tag("!!int")) {
                    storage[full_key] = value.as<int64_t>();
                } else if (is_tag("!!float")) {
                    storage[full_key] = value.as<double>();
                } else if (is_tag("!!bool")) {
                    storage[full_key] = value.as<bool>();
                } else {
                    storage[full_key] = value.as<std::string>();
                }
            } else if (value.IsSequence()) {
                std::vector<std::string> arr;
                bool all_scalar = true;
                for (const auto& item : value) {
                    if (item.IsScalar()) {
                        arr.push_back(item.as<std::string>());
                    } else {
                        all_scalar = false;
                        break;
                    }
                }
                if (all_scalar) {
                    storage[full_key] = arr;
                }
            } else {
                flatten(value, full_key);
            }
        }
    };

    flatten(root, "");
}
}  // namespace

nlohmann::json yaml_to_json(const YAML::Node& node) {
    if (!node || node.IsNull()) {  // GCOVR_EXCL_BR_LINE (defensive: all call
                                   // sites pass guarded, defined nodes; null
                                   // scalars take the covered IsNull arm)
        return nullptr;
    }
    if (node.IsScalar()) {
        try {
            return node.as<bool>();
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // conversion-failure arm is covered)
        }
        try {
            return node.as<std::int64_t>();
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // conversion-failure arm is covered)
        }
        try {
            return node.as<double>();
        } catch (const std::exception&) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: catch blocks have no
                                           // non-exception entry edge; the
                                           // conversion-failure arm is covered)
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
    if (node.IsMap()) {  // GCOVR_EXCL_BR_LINE (defensive: a non-null YAML node
                         // is always scalar, sequence or map; mirrors the
                         // unreachable return below)
        nlohmann::json object = nlohmann::json::object();
        for (const auto& item : node) {
            object[item.first.as<std::string>()] = yaml_to_json(item.second);
        }
        return object;
    }
    return nullptr;  // GCOVR_EXCL_LINE (unreachable: every non-null YAML node
                     // is scalar/sequence/map)
}

bool Config::load_yaml(std::string_view path) {
    try {
        const std::filesystem::path config_path{std::string(path)};
        std::ifstream file(config_path);
        if (!file.is_open()) {
            auto& log = shield::log::get_logger("config");
            SHIELD_LOG_ERROR(
                log, "Failed to open config file: " + std::string(path));
            return false;
        }

        std::unique_lock lock(impl_->mutex);
        YAML::Node loaded = YAML::Load(file);
        impl_->root = merge_yaml_nodes(impl_->root, loaded);
        auto parent = config_path.parent_path();
        impl_->source_dir =
            parent.empty()
                ? "."  // GCOVR_EXCL_BR_LINE (compiler artifact: gcov clone
                       // arms; both ternary arms are covered)
                : parent.string();  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // never-executed gcov clone arms; both
                                    // ternary arms are covered)
        flatten_yaml_node(impl_->root, impl_->storage);

        auto& log = shield::log::get_logger("config");
        SHIELD_LOG_INFO(log, "Loaded config file: " + std::string(path));
        return true;

    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch entry edge)
        const std::exception&
            e) {  // GCOVR_EXCL_BR_LINE (compiler artifact: catch blocks have no
                  // non-exception entry edge; the parse-error arm is covered)
        auto& log = shield::log::get_logger("config");
        SHIELD_LOG_ERROR(log,
                         std::string("Failed to parse config: ") + e.what());
        return false;
    }
}

bool Config::load_yaml_string(std::string_view yaml) {
    try {
        std::unique_lock lock(impl_->mutex);
        YAML::Node root = YAML::Load(std::string(yaml));
        impl_->root = merge_yaml_nodes(impl_->root, root);
        flatten_yaml_node(impl_->root, impl_->storage);
        return true;

    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch entry edge)
        const std::exception&
            e) {  // GCOVR_EXCL_BR_LINE (compiler artifact: catch blocks have no
                  // non-exception entry edge; the parse-error arm is covered)
        return false;
    }
}

}  // namespace shield::config
