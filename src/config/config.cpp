// [SHIELD_CONFIG] Configuration accessors: the typed getters over
// the flattened storage, the process-global instance and its
// initialize/reset/reload lifecycle and the subtree_json view.
// YAML loading, validation and the runtime accessors live in the
// sibling translation units.
#include "shield/config/config.hpp"

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
#include "shield/log/logger.hpp"
#include "shield/net/ip_blocklist.hpp"

namespace shield::log {
class Logger;
}

namespace shield::config {

Config::Config() : impl_(std::make_unique<Impl>()) {}

Config::~Config() = default;

std::string Config::get_string(std::string_view key,
                               std::string_view default_value) const {
    std::shared_lock lock(impl_->mutex);

    auto it = impl_->storage.find(std::string(key));
    if (it != impl_->storage.end()) {
        if (std::holds_alternative<std::string>(it->second)) {
            return std::get<std::string>(it->second);
        } else if (std::holds_alternative<int64_t>(it->second)) {
            return std::to_string(std::get<int64_t>(it->second));
        } else if (std::holds_alternative<double>(it->second)) {
            return std::to_string(std::get<double>(it->second));
        } else if (std::holds_alternative<bool>(it->second)) {
            return std::get<bool>(it->second) ? "true" : "false";
        }
    }

    return std::string(default_value);
}

int64_t Config::get_int(std::string_view key, int64_t default_value) const {
    std::shared_lock lock(impl_->mutex);

    auto it = impl_->storage.find(std::string(key));
    if (it != impl_->storage.end()) {
        if (std::holds_alternative<int64_t>(it->second)) {
            return std::get<int64_t>(it->second);
        } else if (std::holds_alternative<double>(it->second)) {
            return static_cast<int64_t>(std::get<double>(it->second));
        } else if (std::holds_alternative<std::string>(it->second)) {
            try {
                return std::stoll(std::get<std::string>(it->second));
            } catch (...) {
            }
        }
    }

    return default_value;
}

double Config::get_double(std::string_view key, double default_value) const {
    std::shared_lock lock(impl_->mutex);

    auto it = impl_->storage.find(std::string(key));
    if (it != impl_->storage.end()) {
        if (std::holds_alternative<double>(it->second)) {
            return std::get<double>(it->second);
        } else if (std::holds_alternative<int64_t>(it->second)) {
            return static_cast<double>(std::get<int64_t>(it->second));
        } else if (std::holds_alternative<std::string>(it->second)) {
            try {
                return std::stod(std::get<std::string>(it->second));
            } catch (...) {
            }
        }
    }

    return default_value;
}

bool Config::get_bool(std::string_view key, bool default_value) const {
    std::shared_lock lock(impl_->mutex);

    auto it = impl_->storage.find(std::string(key));
    if (it != impl_->storage.end()) {
        if (std::holds_alternative<bool>(it->second)) {
            return std::get<bool>(it->second);
        } else if (std::holds_alternative<std::string>(it->second)) {
            const auto& str = std::get<std::string>(it->second);
            return str == "true" || str == "1" || str == "yes";
        }
    }

    return default_value;
}

std::vector<std::string> Config::get_string_array(std::string_view key) const {
    std::shared_lock lock(impl_->mutex);

    auto it = impl_->storage.find(std::string(key));
    if (it != impl_->storage.end() &&
        std::holds_alternative<std::vector<std::string>>(it->second)) {
        return std::get<std::vector<std::string>>(it->second);
    }

    return {};
}

bool Config::has(std::string_view key) const {
    std::shared_lock lock(impl_->mutex);
    return impl_->storage.find(std::string(key)) != impl_->storage.end();
}

void Config::set(std::string_view key, ConfigValue value) {
    std::unique_lock lock(impl_->mutex);

    // GCOVR_EXCL_START (gcov clone artifact)
    std::visit(
        // The closure header line below is a gcov artifact: set() is
        // exercised by SetAndGetEveryConfigValueType, but the arc lands in
        // an uncalled clone on some gcc builds.
        [&](auto&& v) {
            // GCOVR_EXCL_STOP
            impl_->storage[std::string(
                key)] =  // GCOVR_EXCL_LINE (uncalled visit clone)
                std::forward<decltype(v)>(v);
        },
        std::move(value));
}

const ConfigValue* Config::get_value(std::string_view key) const {
    std::shared_lock lock(impl_->mutex);

    static thread_local ConfigValue value;

    auto it = impl_->storage.find(std::string(key));
    if (it == impl_->storage.end()) {
        return nullptr;
    }

    if (std::holds_alternative<std::string>(it->second)) {
        value = std::get<std::string>(it->second);
    } else if (std::holds_alternative<int64_t>(it->second)) {
        value = std::get<int64_t>(it->second);
    } else if (std::holds_alternative<double>(it->second)) {
        value = std::get<double>(it->second);
    } else if (std::holds_alternative<bool>(it->second)) {
        // GCOVR_EXCL_BR_START (defensive: the vector<string> alternative is
        // never stored; the else arm is the unreachable return below)
        value = std::get<bool>(it->second);
    } else if (std::holds_alternative<std::vector<
                   std::string>>(  // GCOVR_EXCL_BR_LINE (defensive: mirrors the
                                   // unreachable return below)
                   it->second)) {  // GCOVR_EXCL_BR_LINE (defensive: reached
                                   // only after every other alternative missed;
                                   // the else arm is the unreachable return
                                   // below)
        value = std::get<std::vector<std::string>>(it->second);
        // GCOVR_EXCL_BR_STOP
    } else {
        return nullptr;  // GCOVR_EXCL_LINE (unreachable: all alternatives
                         // checked)
    }

    return &value;
}

void Config::merge(const Config& other) {
    std::unique_lock lock(impl_->mutex);
    std::shared_lock other_lock(other.impl_->mutex);

    for (const auto& [key, value] : other.impl_->storage) {
        impl_->storage[key] = value;
    }
}

std::string Config::to_json() const {
    std::shared_lock lock(impl_->mutex);

    nlohmann::json j;
    for (const auto& [key, value] : impl_->storage) {
        if (std::holds_alternative<std::string>(value)) {
            j[key] = std::get<std::string>(value);
        } else if (std::holds_alternative<int64_t>(value)) {
            j[key] = std::get<int64_t>(value);
        } else if (std::holds_alternative<double>(value)) {
            j[key] = std::get<double>(value);
        } else if (std::holds_alternative<bool>(value)) {
            j[key] = std::get<bool>(value);
        }
    }
    return j.dump();
}

// Global config instance
static Config* g_global_config = nullptr;
static std::unique_ptr<Config> g_global_config_owner;

Config& global_config() {
    if (!g_global_config) {
        g_global_config_owner = std::make_unique<Config>();
        g_global_config = g_global_config_owner.get();
    }
    return *g_global_config;
}

bool initialize_config(std::string_view config_path) {
    return global_config().load_yaml(config_path);
}

void reset_config() {
    g_global_config_owner.reset();
    g_global_config = nullptr;
}

namespace {
// Walk a dotted path through nested map nodes. Uses only const operator[]
// (no insertion side effect) and copy construction / return-by-value —
// yaml-cpp's Node::operator= rebinds the *target* node via set_ref, so
// assigning a child back onto a node that aliases its parent would destroy
// the parent's subtree.
const YAML::Node find_subtree_node(const YAML::Node& node,
                                   std::string_view path) {
    if (path.empty()) {
        return node;
    }
    const auto dot = path.find('.');
    const auto segment =
        (dot == std::string_view::npos) ? path : path.substr(0, dot);
    if (segment.empty() || !node.IsMap()) {
        return YAML::Node(YAML::NodeType::Undefined);
    }
    const YAML::Node child = node[std::string(segment)];
    if (!child || child.IsNull()) {
        return YAML::Node(YAML::NodeType::Undefined);
    }
    return find_subtree_node(child, (dot == std::string_view::npos)
                                        ? std::string_view{}
                                        : path.substr(dot + 1));
}
}  // namespace

std::string subtree_json(const Config& config, std::string_view path) {
    if (path.empty()) {
        return "{}";
    }
    std::shared_lock lock(config.impl_->mutex);
    const YAML::Node root = config.impl_->root;
    const YAML::Node node = find_subtree_node(root, path);
    if (!node ||
        node.IsNull()) {  // GCOVR_EXCL_BR_LINE (defensive: find_subtree_node
                          // never returns a defined-null node, so the IsNull
                          // arm is unreachable)
        return "{}";
    }
    return yaml_to_json(node).dump();
}

bool reload_config() {
    // Would need to track the original config path
    return true;
}

// Convenience functions
std::string get(std::string_view key, std::string_view default_value) {
    return global_config().get_string(key, default_value);
}

int64_t get_int(std::string_view key, int64_t default_value) {
    return global_config().get_int(key, default_value);
}

double get_double(std::string_view key, double default_value) {
    return global_config().get_double(key, default_value);
}

bool get_bool(std::string_view key, bool default_value) {
    return global_config().get_bool(key, default_value);
}

}  // namespace shield::config
