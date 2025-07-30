#pragma once

#include <yaml-cpp/yaml.h>  // Keep for YAML parsing utility

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace shield::config {

// Configuration file path constants
class ConfigPaths {
public:
    static constexpr const char* DEFAULT_CONFIG_FILE = "config/app.yaml";
    static constexpr const char* DEFAULT_CONFIG_DIR = "config/";

    static std::string get_profile_config_file(const std::string& profile) {
        return std::string(DEFAULT_CONFIG_DIR) + "shield-" + profile + ".yaml";
    }

    static std::string get_config_dir() { return DEFAULT_CONFIG_DIR; }
};

enum class ConfigFormat { YAML, JSON, INI };

// Configuration properties base class (equivalent to Spring Boot's
// @ConfigurationProperties)
class ConfigurationProperties {
public:
    virtual ~ConfigurationProperties() = default;
    virtual void from_ptree(
        const boost::property_tree::ptree& pt) = 0;  // Load from property tree
    virtual void validate() const {}
    virtual std::string properties_name() const = 0;  // Name of the properties
    virtual bool supports_hot_reload() const { return false; }
    virtual std::unique_ptr<ConfigurationProperties> clone()
        const = 0;  // Clone for hot reload
protected:
    // Helper methods for parsing ptree
    template <typename T>
    T get_value(const boost::property_tree::ptree& pt, const std::string& path,
                const T& default_value) {
        return pt.get<T>(path, default_value);
    }

    template <typename T>
    std::optional<T> get_optional_value(const boost::property_tree::ptree& pt,
                                        const std::string& path) {
        auto result = pt.get_optional<T>(path);
        if (result) {
            return *result;
        }
        return std::nullopt;
    }

    template <typename T>
    T get_required_value(const boost::property_tree::ptree& pt,
                         const std::string& path) {
        try {
            return pt.get<T>(path);
        } catch (const boost::property_tree::ptree_bad_path& e) {
            throw std::runtime_error("Missing required config value: " + path +
                                     ". Error: " + e.what());
        }
    }

    template <typename T>
    void load_vector(const boost::property_tree::ptree& pt,
                     const std::string& path, std::vector<T>& vec) {
        vec.clear();
        if (auto child_pt = pt.get_child_optional(path)) {
            for (const auto& v : *child_pt) {
                vec.push_back(v.second.get_value<T>());
            }
        }
    }
};

// CRTP template for providing automatic clone() implementation
template <typename Derived>
class ClonableConfigurationProperties : public ConfigurationProperties {
public:
    std::unique_ptr<ConfigurationProperties> clone() const override {
        return std::make_unique<Derived>(static_cast<const Derived&>(*this));
    }
};

// Template for configuration properties that support hot reloading
template <typename Derived>
class ReloadableConfigurationProperties
    : public ClonableConfigurationProperties<Derived> {
public:
    bool supports_hot_reload() const override { return true; }
};

// Configuration manager
class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager instance;
        return instance;
    }

    // Load configuration file
    void load_config(const std::string& config_file,
                     ConfigFormat format = ConfigFormat::YAML);
    void load_config_with_profile(const std::string& profile = "",
                                  ConfigFormat format = ConfigFormat::YAML);

    // Reload configuration file and notify components to update
    void reload_config(const std::string& config_file,
                       ConfigFormat format = ConfigFormat::YAML);

    // Register configuration properties
    template <typename T>
    void register_configuration_properties(std::shared_ptr<T> config) {
        static_assert(std::is_base_of_v<ConfigurationProperties, T>,
                      "T must inherit from ConfigurationProperties");
        const std::type_index type_id = std::type_index(typeid(T));
        configs_[type_id] = config;
        config_by_name_[config->properties_name()] = config;
    }

    // Get configuration properties
    template <typename T>
    std::shared_ptr<T> get_configuration_properties() const {
        const std::type_index type_id = std::type_index(typeid(T));
        auto it = configs_.find(type_id);
        if (it != configs_.end()) {
            return std::static_pointer_cast<T>(it->second);
        }
        return nullptr;
    }

    // Get configuration properties by name
    std::shared_ptr<ConfigurationProperties> get_config_by_name(
        const std::string& name) const {
        auto it = config_by_name_.find(name);
        return (it != config_by_name_.end()) ? it->second : nullptr;
    }

    // Reset all configurations
    void reset() {
        configs_.clear();
        config_by_name_.clear();
        config_tree_ = boost::property_tree::ptree();
    }

    // Get raw property_tree node (for debugging)
    const boost::property_tree::ptree& get_config_tree() const {
        return config_tree_;
    }

    // Subscribe to configuration reload events for a specific config type.
    // The callback will be invoked after a successful reload.
    template <typename ConfigType>
    void subscribe_to_reloads(
        std::function<void(const ConfigType& new_config)> callback) {
        static_assert(
            std::is_base_of_v<ReloadableConfigurationProperties<ConfigType>,
                              ConfigType>,
            "Can only subscribe to types derived from "
            "ReloadableConfigurationProperties");

        ReloadCallback generic_callback =
            [cb = std::move(callback)](const ConfigurationProperties& config) {
                cb(static_cast<const ConfigType&>(config));
            };

        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        reload_subscribers_[std::type_index(typeid(ConfigType))].push_back(
            std::move(generic_callback));
    }

private:
    ConfigManager() = default;

    using ReloadCallback =
        std::function<void(const ConfigurationProperties& new_config)>;

    std::mutex
        config_mutex_;  // To protect configs_ and config_tree_ during swaps
    std::unordered_map<std::type_index,
                       std::shared_ptr<ConfigurationProperties>>
        configs_;
    std::unordered_map<std::string, std::shared_ptr<ConfigurationProperties>>
        config_by_name_;
    boost::property_tree::ptree config_tree_;

    std::mutex subscribers_mutex_;  // To protect the subscribers map
    std::unordered_map<std::type_index, std::vector<ReloadCallback>>
        reload_subscribers_;

    // Merge property_tree nodes
    boost::property_tree::ptree merge_ptrees(
        const boost::property_tree::ptree& base,
        const boost::property_tree::ptree& override);

    // Load component configurations (now also used for re-applying on reload)
    void load_component_configs(bool is_reload = false);

    // Helper to convert YAML::Node to boost::property_tree::ptree
    boost::property_tree::ptree yaml_to_ptree(const YAML::Node& node);
};

// Configuration properties factory
template <typename T>
class ConfigurationPropertiesFactory {
public:
    static std::shared_ptr<T> create_and_register() {
        auto config = std::make_shared<T>();
        ConfigManager::instance().register_configuration_properties(config);
        return config;
    }
};

// Configuration properties initialization macro, simplifies registration
// process
#define REGISTER_CONFIGURATION_PROPERTIES(ConfigType)   \
    namespace {                                         \
    [[maybe_unused]] static auto _ = []() {             \
        shield::config::ConfigurationPropertiesFactory< \
            ConfigType>::create_and_register();         \
        return 0;                                       \
    }();                                                \
    }

}  // namespace shield::config
