#pragma once

#include <yaml-cpp/yaml.h>

#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace shield::config {

// 模块配置基类
class ModuleConfig {
public:
    virtual ~ModuleConfig() = default;
    virtual void from_yaml(const YAML::Node& node) = 0;
    virtual YAML::Node to_yaml() const = 0;
    virtual void validate() const {}
    virtual std::string module_name() const = 0;
};

// 配置管理器
class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager instance;
        return instance;
    }

    // 加载配置文件
    void load_config(const std::string& config_file);
    void load_config_with_profile(const std::string& profile = "");

    // 注册模块配置
    template <typename T>
    void register_module_config(std::shared_ptr<T> config) {
        static_assert(std::is_base_of_v<ModuleConfig, T>,
                      "T must inherit from ModuleConfig");
        const std::type_index type_id = std::type_index(typeid(T));
        configs_[type_id] = config;
        config_by_name_[config->module_name()] = config;
    }

    // 获取模块配置
    template <typename T>
    std::shared_ptr<T> get_module_config() const {
        const std::type_index type_id = std::type_index(typeid(T));
        auto it = configs_.find(type_id);
        if (it != configs_.end()) {
            return std::static_pointer_cast<T>(it->second);
        }
        return nullptr;
    }

    // 通过名称获取配置
    std::shared_ptr<ModuleConfig> get_config_by_name(
        const std::string& name) const {
        auto it = config_by_name_.find(name);
        return (it != config_by_name_.end()) ? it->second : nullptr;
    }

    // 重置所有配置
    void reset() {
        configs_.clear();
        config_by_name_.clear();
        yaml_config_ = YAML::Node();
    }

    // 获取原始YAML节点（用于调试）
    const YAML::Node& get_yaml_config() const { return yaml_config_; }

private:
    ConfigManager() = default;

    std::unordered_map<std::type_index, std::shared_ptr<ModuleConfig>> configs_;
    std::unordered_map<std::string, std::shared_ptr<ModuleConfig>>
        config_by_name_;
    YAML::Node yaml_config_;

    // 合并YAML节点
    YAML::Node merge_yaml_nodes(const YAML::Node& base,
                                const YAML::Node& override);

    // 加载模块配置
    void load_module_configs();
};

// 模块配置工厂
template <typename T>
class ModuleConfigFactory {
public:
    static std::shared_ptr<T> create_and_register() {
        auto config = std::make_shared<T>();
        ConfigManager::instance().register_module_config(config);
        return config;
    }
};

// 配置初始化宏，简化注册过程
#define REGISTER_MODULE_CONFIG(ConfigType)      \
    namespace {                                 \
    [[maybe_unused]] static auto _ = []() {     \
        shield::config::ModuleConfigFactory<    \
            ConfigType>::create_and_register(); \
        return 0;                               \
    }();                                        \
    }

}  // namespace shield::config