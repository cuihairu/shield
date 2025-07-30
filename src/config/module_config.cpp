#include "shield/config/module_config.hpp"

#include <filesystem>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

namespace shield::config {

void ConfigManager::load_config(const std::string& config_file) {
    SHIELD_LOG_INFO << "Loading config file: " << config_file;

    try {
        yaml_config_ = YAML::LoadFile(config_file);
        load_module_configs();
        SHIELD_LOG_INFO << "Successfully loaded config file: " << config_file;
    } catch (const YAML::Exception& e) {
        SHIELD_LOG_ERROR << "Failed to load config file: " << config_file
                         << ", Error: " << e.what();
        throw std::runtime_error("Failed to load config file: " + config_file +
                                 ", Error: " + e.what());
    }
}

void ConfigManager::load_config_with_profile(const std::string& profile) {
    YAML::Node base_config;

    // 加载基础配置
    const std::string base_file = ConfigPaths::DEFAULT_CONFIG_FILE;
    try {
        base_config = YAML::LoadFile(base_file);
        SHIELD_LOG_INFO << "Loaded base configuration: " << base_file;
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to load base config: " +
                                 std::string(e.what()));
    }

    // 如果指定了profile，加载profile配置并合并
    if (!profile.empty()) {
        std::string profile_file =
            ConfigPaths::get_profile_config_file(profile);

        if (!std::filesystem::exists(profile_file)) {
            throw std::runtime_error("Profile config file not found: " +
                                     profile_file);
        }

        try {
            YAML::Node profile_config = YAML::LoadFile(profile_file);
            SHIELD_LOG_INFO << "Loaded profile configuration: " << profile_file;
            base_config = merge_yaml_nodes(base_config, profile_config);
        } catch (const YAML::Exception& e) {
            throw std::runtime_error("Failed to load profile config: " +
                                     profile_file + ", Error: " + e.what());
        }
    }

    yaml_config_ = base_config;
    load_module_configs();
    SHIELD_LOG_INFO << "Configuration loaded successfully"
                    << (profile.empty() ? "" : " with profile: " + profile);
}

YAML::Node ConfigManager::merge_yaml_nodes(const YAML::Node& base,
                                           const YAML::Node& override) {
    YAML::Node result = YAML::Clone(base);

    for (const auto& item : override) {
        const std::string& key = item.first.as<std::string>();
        const YAML::Node& value = item.second;

        if (result[key] && result[key].IsMap() && value.IsMap()) {
            result[key] = merge_yaml_nodes(result[key], value);
        } else {
            result[key] = value;
        }
    }

    return result;
}

void ConfigManager::load_module_configs() {
    // 为所有已注册的模块配置加载数据
    for (auto& [type_id, config] : configs_) {
        const std::string& module_name = config->module_name();

        if (yaml_config_[module_name]) {
            try {
                config->from_yaml(yaml_config_[module_name]);
                config->validate();
                SHIELD_LOG_DEBUG << "Loaded configuration for module: "
                                 << module_name;
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "Failed to load configuration for module "
                                 << module_name << ": " << e.what();
                throw;
            }
        } else {
            SHIELD_LOG_WARN
                << "No configuration found for module: " << module_name
                << ", using defaults";
        }
    }
}

}  // namespace shield::config