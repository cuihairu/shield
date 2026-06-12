// [SHIELD_CONFIG] Configuration implementation
#include "shield/config/config.hpp"

#include "shield/log/logger.hpp"

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

namespace shield::log {
class Logger;
}

namespace shield::config {

// Internal storage type
using StorageValue = std::variant<
    std::string,
    int64_t,
    double,
    bool,
    std::vector<std::string>,
    YAML::Node
>;

struct Config::Impl {
    std::unordered_map<std::string, StorageValue> storage;
    mutable std::shared_mutex mutex;
    YAML::Node root;  // Keep original YAML for complex queries
    std::string source_dir;
};

Config::Config() : impl_(std::make_unique<Impl>()) {}

Config::~Config() = default;

namespace {

// Helper: split key by dots
std::vector<std::string> split_key(std::string_view key) {
    std::vector<std::string> result;
    std::string current;
    for (char c : key) {
        if (c == '.') {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

// Helper: navigate YAML tree by key path
YAML::Node navigate_yaml(const YAML::Node& node,
                         const std::vector<std::string>& path) {
    YAML::Node current = node;
    for (const auto& key : path) {
        if (!current.IsMap() || !current[key]) {
            return YAML::Node();
        }
        current = current[key];
    }
    return current;
}

YAML::Node merge_yaml_nodes(const YAML::Node& base, const YAML::Node& overlay) {
    if (!base || base.IsNull()) {
        return YAML::Clone(overlay);
    }
    if (!overlay || overlay.IsNull()) {
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
            std::string key = it->first.as<std::string>();
            std::string full_key = prefix.empty() ? key : prefix + "." + key;
            const YAML::Node& value = it->second;

            if (value.IsScalar()) {
                if (value.Tag() == "!!int") {
                    storage[full_key] = value.as<int64_t>();
                } else if (value.Tag() == "!!float") {
                    storage[full_key] = value.as<double>();
                } else if (value.Tag() == "!!bool") {
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

std::optional<int> scalar_int(const YAML::Node& node, const char* key) {
    if (!node || !node[key]) {
        return std::nullopt;
    }
    try {
        return node[key].as<int>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool scalar_bool_default(const YAML::Node& node, const char* key, bool fallback) {
    if (!node || !node[key]) {
        return fallback;
    }
    try {
        return node[key].as<bool>();
    } catch (const std::exception&) {
        return fallback;
    }
}

bool validate_port_range(const YAML::Node& node,
                         const char* path,
                         std::string* error) {
    if (!node) {
        return true;
    }
    auto port = scalar_int(node, "port");
    if (port && (*port < 1 || *port > 65535)) {
        if (error) {
            *error = std::string(path) + ".port must be between 1 and 65535";
        }
        return false;
    }
    return true;
}

bool validate_pool_sizes(const YAML::Node& node,
                         const char* path,
                         std::string* error) {
    if (!node) {
        return true;
    }

    const auto pool_size = scalar_int(node, "pool_size");
    const auto max_pool_size = scalar_int(node, "max_pool_size");
    if (pool_size && *pool_size < 1) {
        if (error) {
            *error = std::string(path) + ".pool_size must be >= 1";
        }
        return false;
    }
    if (max_pool_size && *max_pool_size < 1) {
        if (error) {
            *error = std::string(path) + ".max_pool_size must be >= 1";
        }
        return false;
    }
    if (pool_size && max_pool_size && *pool_size > *max_pool_size) {
        if (error) {
            *error = std::string(path) + ".pool_size must be <= max_pool_size";
        }
        return false;
    }
    return true;
}

bool validate_listener_address(const YAML::Node& node,
                               const char* key,
                               const std::string& actor_name,
                               std::string* error) {
    if (!node[key]) {
        return true;
    }

    std::string value;
    try {
        value = node[key].as<std::string>();
    } catch (const std::exception&) {
        if (error) {
            *error = "actors[" + actor_name + "].network." + key +
                     " must be a string host:port";
        }
        return false;
    }

    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
        if (error) {
            *error = "actors[" + actor_name + "].network." + key +
                     " must be host:port";
        }
        return false;
    }

    try {
        const int port = std::stoi(value.substr(colon + 1));
        if (port < 1 || port > 65535) {
            throw std::out_of_range("port");
        }
    } catch (const std::exception&) {
        if (error) {
            *error = "actors[" + actor_name + "].network." + key +
                     " port must be between 1 and 65535";
        }
        return false;
    }

    return true;
}

nlohmann::json yaml_to_json(const YAML::Node& node) {
    if (!node || node.IsNull()) {
        return nullptr;
    }
    if (node.IsScalar()) {
        try {
            return node.as<bool>();
        } catch (const std::exception&) {}
        try {
            return node.as<std::int64_t>();
        } catch (const std::exception&) {}
        try {
            return node.as<double>();
        } catch (const std::exception&) {}
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

bool Config::load_yaml(std::string_view path) {
    try {
        std::ifstream file{std::string(path)};
        if (!file.is_open()) {
            auto& log = shield::log::get_logger("config");
            SHIELD_LOG_ERROR(log, "Failed to open config file: " + std::string(path));
            return false;
        }

        std::unique_lock lock(impl_->mutex);
        YAML::Node loaded = YAML::Load(file);
        impl_->root = merge_yaml_nodes(impl_->root, loaded);
        auto parent = std::filesystem::path(std::string(path)).parent_path();
        impl_->source_dir = parent.empty() ? "." : parent.string();
        flatten_yaml_node(impl_->root, impl_->storage);

        auto& log = shield::log::get_logger("config");
        SHIELD_LOG_INFO(log, "Loaded config file: " + std::string(path));
        return true;

    } catch (const std::exception& e) {
        auto& log = shield::log::get_logger("config");
        SHIELD_LOG_ERROR(log, std::string("Failed to parse config: ") + e.what());
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

    } catch (const std::exception& e) {
        return false;
    }
}

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
            } catch (...) {}
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
            } catch (...) {}
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

    std::visit(
        [&](auto&& v) {
            impl_->storage[std::string(key)] = std::forward<decltype(v)>(v);
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
        value = std::get<bool>(it->second);
    } else if (std::holds_alternative<std::vector<std::string>>(it->second)) {
        value = std::get<std::vector<std::string>>(it->second);
    } else {
        return nullptr;
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

bool validate_runtime_config(const RuntimeValidationOptions& options,
                             std::string* error) {
    auto& config = global_config();
    std::shared_lock lock(config.impl_->mutex);
    const YAML::Node& root = config.impl_->root;

    const struct OptionalModule {
        const char* section;
        const char* module;
        bool enabled;
    } optional_modules[] = {
        {"cluster", "shield_cluster", options.cluster_enabled},
        {"global", "shield_global", options.global_enabled},
        {"player", "shield_player", options.player_enabled},
        {"server_manager", "shield_server", options.server_enabled},
        {"ops", "shield_ops", options.ops_enabled},
    };

    for (const auto& module : optional_modules) {
        if (root[module.section] && !module.enabled) {
            if (error) {
                *error = std::string("optional module config '") +
                         module.section + "' requires " + module.module;
            }
            return false;
        }
    }

    if (!root["app"] || !root["app"].IsMap() || !root["app"]["name"] ||
        root["app"]["name"].as<std::string>().empty()) {
        if (error) {
            *error = "app.name is required";
        }
        return false;
    }

    if (options.require_actors) {
        const YAML::Node actors = root["actors"];
        if (!actors || !actors.IsSequence() || actors.size() == 0) {
            if (error) {
                *error = "actors must contain at least one actor";
            }
            return false;
        }

        std::unordered_set<std::string> names;
        for (std::size_t i = 0; i < actors.size(); ++i) {
            const YAML::Node actor = actors[i];
            if (!actor.IsMap()) {
                if (error) {
                    *error = "actors[" + std::to_string(i) + "] must be a map";
                }
                return false;
            }

            std::string name;
            try {
                name = actor["name"].as<std::string>();
            } catch (const std::exception&) {
                if (error) {
                    *error = "actors[" + std::to_string(i) + "].name is required";
                }
                return false;
            }
            if (name.empty()) {
                if (error) {
                    *error = "actors[" + std::to_string(i) + "].name is required";
                }
                return false;
            }
            if (!names.insert(name).second) {
                if (error) {
                    *error = "actors[].name must be unique: " + name;
                }
                return false;
            }

            if (!actor["script"] || actor["script"].as<std::string>().empty()) {
                if (error) {
                    *error = "actors[" + name + "].script is required";
                }
                return false;
            }

            if (auto instances = scalar_int(actor, "instances");
                instances && *instances < 0) {
                if (error) {
                    *error = "actors[" + name + "].instances must be >= 0";
                }
                return false;
            }

            if (const YAML::Node restart = actor["restart"];
                restart && restart["policy"]) {
                const auto policy = restart["policy"].as<std::string>();
                if (policy != "always" && policy != "on-failure" &&
                    policy != "never") {
                    if (error) {
                        *error = "actors[" + name +
                                 "].restart.policy must be always, on-failure, or never";
                    }
                    return false;
                }
            }

            if (const YAML::Node network = actor["network"]) {
                if (!network.IsMap()) {
                    if (error) {
                        *error = "actors[" + name + "].network must be a map";
                    }
                    return false;
                }

                if (network["udp"] || network["kcp"] || network["websocket"]) {
                    if (error) {
                        *error = "actors[" + name +
                                 "].network only supports tcp in Phase 1";
                    }
                    return false;
                }

                if (!validate_listener_address(network, "tcp", name, error)) {
                    return false;
                }
            }
        }
    }

    if (const YAML::Node database = root["database"]) {
        if (database.IsMap() &&
            scalar_bool_default(database, "enabled", true)) {
            if (!validate_port_range(database, "database", error) ||
                !validate_pool_sizes(database, "database", error)) {
                return false;
            }
        }
    }

    if (const YAML::Node redis = root["redis"]) {
        if (redis.IsMap() && scalar_bool_default(redis, "enabled", true)) {
            if (!validate_port_range(redis, "redis", error) ||
                !validate_pool_sizes(redis, "redis", error)) {
                return false;
            }
        }
    }

    return true;
}

void reset_config() {
    g_global_config_owner.reset();
    g_global_config = nullptr;
}

std::vector<RuntimeActorConfig> runtime_actors() {
    auto& config = global_config();
    std::shared_lock lock(config.impl_->mutex);

    std::vector<RuntimeActorConfig> result;
    const YAML::Node actors = config.impl_->root["actors"];
    if (!actors || !actors.IsSequence()) {
        return result;
    }

    for (const auto& actor : actors) {
        RuntimeActorConfig item;
        item.name = actor["name"].as<std::string>();
        item.script = actor["script"].as<std::string>();
        item.source_dir = config.impl_->source_dir;
        item.instances = scalar_int(actor, "instances").value_or(1);
        item.required = scalar_bool_default(actor, "required", true);

        if (actor["options"]) {
            item.options_json = yaml_to_json(actor["options"]).dump();
        }

        result.push_back(std::move(item));
    }

    return result;
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
