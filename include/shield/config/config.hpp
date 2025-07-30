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

// 配置文件路径常量
class ConfigPaths {
public:
    static constexpr const char* DEFAULT_CONFIG_FILE = "config/shield.yaml";
    static constexpr const char* DEFAULT_CONFIG_DIR = "config/";

    static std::string get_profile_config_file(const std::string& profile) {
        return std::string(DEFAULT_CONFIG_DIR) + "shield-" + profile + ".yaml";
    }

    static std::string get_config_dir() { return DEFAULT_CONFIG_DIR; }
};

enum class ConfigFormat { YAML, JSON, INI };

// 模块配置基类
class ComponentConfig {
public:
    virtual ~ComponentConfig() = default;
    virtual void from_ptree(
        const boost::property_tree::ptree& pt) = 0;  // Changed to from_ptree
    virtual YAML::Node to_yaml()
        const = 0;  // Keep to_yaml for now, might be removed later
    virtual void validate() const {}
    virtual std::string component_name() const = 0;
    virtual bool supports_hot_reload() const { return false; }
    virtual std::unique_ptr<ComponentConfig> clone()
        const = 0;  // Add clone method
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

// 配置管理器
class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager instance;
        return instance;
    }

    // 加载配置文件
    void load_config(const std::string& config_file,
                     ConfigFormat format = ConfigFormat::YAML);
    void load_config_with_profile(const std::string& profile = "",
                                  ConfigFormat format = ConfigFormat::YAML);

    // 重新加载配置文件并通知组件更新
    void reload_config(const std::string& config_file,
                       ConfigFormat format = ConfigFormat::YAML);

    // 注册组件配置
    template <typename T>
    void register_component_config(std::shared_ptr<T> config) {
        static_assert(std::is_base_of_v<ComponentConfig, T>,
                      "T must inherit from ComponentConfig");
        const std::type_index type_id = std::type_index(typeid(T));
        configs_[type_id] = config;
        config_by_name_[config->component_name()] = config;
    }

    // 获取组件配置
    template <typename T>
    std::shared_ptr<T> get_component_config() const {
        const std::type_index type_id = std::type_index(typeid(T));
        auto it = configs_.find(type_id);
        if (it != configs_.end()) {
            return std::static_pointer_cast<T>(it->second);
        }
        return nullptr;
    }

    // 通过名称获取配置
    std::shared_ptr<ComponentConfig> get_config_by_name(
        const std::string& name) const {
        auto it = config_by_name_.find(name);
        return (it != config_by_name_.end()) ? it->second : nullptr;
    }

    // 重置所有配置
    void reset() {
        configs_.clear();
        config_by_name_.clear();
        config_tree_ = boost::property_tree::ptree();
    }

    // 获取原始property_tree节点（用于调试）
    const boost::property_tree::ptree& get_config_tree() const {
        return config_tree_;
    }

private:
    ConfigManager() = default;

    std::unordered_map<std::type_index, std::shared_ptr<ComponentConfig>>
        configs_;
    std::unordered_map<std::string, std::shared_ptr<ComponentConfig>>
        config_by_name_;
    boost::property_tree::ptree config_tree_;  // Changed from YAML::Node

    // 合并property_tree节点
    boost::property_tree::ptree merge_ptrees(
        const boost::property_tree::ptree& base,
        const boost::property_tree::ptree& override);

    // 加载组件配置 (now also used for re-applying on reload)
    void load_component_configs(bool is_reload = false);

    // Helper to convert YAML::Node to boost::property_tree::ptree
    boost::property_tree::ptree yaml_to_ptree(const YAML::Node& node);
};

// 组件配置工厂
template <typename T>
class ComponentConfigFactory {
public:
    static std::shared_ptr<T> create_and_register() {
        auto config = std::make_shared<T>();
        ConfigManager::instance().register_component_config(config);
        return config;
    }
};

// 配置初始化宏，简化注册过程
#define REGISTER_COMPONENT_CONFIG(ConfigType)   \
    namespace {                                 \
    [[maybe_unused]] static auto _ = []() {     \
        shield::config::ComponentConfigFactory< \
            ConfigType>::create_and_register(); \
        return 0;                               \
    }();                                        \
    }

// Helper macro for clone implementation
#define CLONE_IMPL(ClassName)                                 \
    std::unique_ptr<ComponentConfig> clone() const override { \
        return std::make_unique<ClassName>(*this);            \
    }

}  // namespace shield::config
