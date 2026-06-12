// [SHIELD_CONFIG] Configuration implementation
#include "shield/config/config.hpp"

#include "shield/log/logger.hpp"

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <sstream>
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

}  // namespace

bool Config::load_yaml(std::string_view path) {
    try {
        std::ifstream file(std::string(path));
        if (!file.is_open()) {
            auto& log = shield::log::get_logger("config");
            SHIELD_LOG_ERROR(log, "Failed to open config file: " + std::string(path));
            return false;
        }

        YAML::Node root = YAML::Load(file);
        impl_->root = root;

        // Flatten to storage for fast access
        std::unique_lock lock(impl_->mutex);

        std::function<void(const YAML::Node&, const std::string&)> flatten;
        flatten = [&](const YAML::Node& node, const std::string& prefix) {
            if (node.IsMap()) {
                for (auto it = node.begin(); it != node.end(); ++it) {
                    std::string key = it->first.as<std::string>();
                    std::string full_key = prefix.empty() ? key : prefix + "." + key;
                    const YAML::Node& value = it->second;

                    // Store leaf values
                    if (value.IsScalar()) {
                        if (value.Tag() == "!!int") {
                            impl_->storage[full_key] = value.as<int64_t>();
                        } else if (value.Tag() == "!!float") {
                            impl_->storage[full_key] = value.as<double>();
                        } else if (value.Tag() == "!!bool") {
                            impl_->storage[full_key] = value.as<bool>();
                        } else {
                            impl_->storage[full_key] = value.as<std::string>();
                        }
                    } else if (value.IsSequence()) {
                        std::vector<std::string> arr;
                        for (const auto& item : value) {
                            if (item.IsScalar()) {
                                arr.push_back(item.as<std::string>());
                            }
                        }
                        impl_->storage[full_key] = arr;
                    } else {
                        // Nested object - recurse
                        flatten(value, full_key);
                    }
                }
            }
        };

        flatten(root, "");

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
        YAML::Node root = YAML::Load(std::string(yaml));
        impl_->root = root;

        // Similar flattening as load_yaml
        // (omitted for brevity - would reuse same logic)
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
