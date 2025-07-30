#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <fstream>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

namespace shield::config {

// Helper to convert YAML::Node to boost::property_tree::ptree
boost::property_tree::ptree ConfigManager::yaml_to_ptree(
    const YAML::Node& node) {
    boost::property_tree::ptree pt;
    if (node.IsMap()) {
        for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
            pt.add_child(it->first.as<std::string>(),
                         yaml_to_ptree(it->second));
        }
    } else if (node.IsSequence()) {
        for (YAML::const_iterator it = node.begin(); it != node.end(); ++it) {
            pt.add_child("",
                         yaml_to_ptree(*it));  // Empty key for array elements
        }
    } else if (node.IsScalar()) {
        pt.put("", node.as<std::string>());
    }
    return pt;
}

void ConfigManager::load_config(const std::string& config_file,
                                ConfigFormat format) {
    SHIELD_LOG_INFO << "Loading config file: " << config_file;

    try {
        switch (format) {
            case ConfigFormat::YAML: {
                YAML::Node yaml_node = YAML::LoadFile(config_file);
                config_tree_ = yaml_to_ptree(yaml_node);
                break;
            }
            case ConfigFormat::JSON: {
                std::ifstream ifs(config_file);
                boost::property_tree::read_json(ifs, config_tree_);
                break;
            }
            case ConfigFormat::INI: {
                std::ifstream ifs(config_file);
                boost::property_tree::read_ini(ifs, config_tree_);
                break;
            }
        }
        load_component_configs(false);
        SHIELD_LOG_INFO << "Successfully loaded config file: " << config_file;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to load config file: " << config_file
                         << ", Error: " << e.what();
        throw std::runtime_error("Failed to load config file: " + config_file +
                                 ", Error: " + e.what());
    }
}

void ConfigManager::load_config_with_profile(const std::string& profile,
                                             ConfigFormat format) {
    boost::property_tree::ptree base_ptree;

    // 加载基础配置
    const std::string base_file = ConfigPaths::DEFAULT_CONFIG_FILE;
    try {
        switch (format) {
            case ConfigFormat::YAML: {
                YAML::Node yaml_node = YAML::LoadFile(base_file);
                base_ptree = yaml_to_ptree(yaml_node);
                break;
            }
            case ConfigFormat::JSON: {
                std::ifstream ifs(base_file);
                boost::property_tree::read_json(ifs, base_ptree);
                break;
            }
            case ConfigFormat::INI: {
                std::ifstream ifs(base_file);
                boost::property_tree::read_ini(ifs, base_ptree);
                break;
            }
        }
        SHIELD_LOG_INFO << "Loaded base configuration: " << base_file;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to load base config: " << base_file
                         << ", Error: " << e.what();
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
            boost::property_tree::ptree profile_ptree;
            switch (format) {
                case ConfigFormat::YAML: {
                    YAML::Node yaml_node = YAML::LoadFile(profile_file);
                    profile_ptree = yaml_to_ptree(yaml_node);
                    break;
                }
                case ConfigFormat::JSON: {
                    std::ifstream ifs(profile_file);
                    boost::property_tree::read_json(ifs, profile_ptree);
                    break;
                }
                case ConfigFormat::INI: {
                    std::ifstream ifs(profile_file);
                    boost::property_tree::read_ini(ifs, profile_ptree);
                    break;
                }
            }
            SHIELD_LOG_INFO << "Loaded profile configuration: " << profile_file;
            base_ptree = merge_ptrees(base_ptree, profile_ptree);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR
                << "Failed to load profile config: " << profile_file
                << ", Error: " << e.what();
            throw std::runtime_error("Failed to load profile config: " +
                                     profile_file + ", Error: " + e.what());
        }
    }

    config_tree_ = base_ptree;
    load_component_configs(false);
    SHIELD_LOG_INFO << "Configuration loaded successfully"
                    << (profile.empty() ? "" : " with profile: " + profile);
}

boost::property_tree::ptree ConfigManager::merge_ptrees(
    const boost::property_tree::ptree& base,
    const boost::property_tree::ptree& override) {
    boost::property_tree::ptree result = base;

    for (const auto& item : override) {
        if (result.count(item.first) && !item.second.empty() &&
            !result.get_child(item.first).empty()) {
            result.put_child(
                item.first,
                merge_ptrees(result.get_child(item.first), item.second));
        } else {
            result.put_child(item.first, item.second);
        }
    }

    return result;
}

void ConfigManager::load_component_configs(bool is_reload) {
    // 为所有已注册的组件配置加载数据
    for (auto& [type_id, config] : configs_) {
        if (is_reload && !config->supports_hot_reload()) {
            SHIELD_LOG_DEBUG << "Component " << config->component_name()
                             << " does not support hot reload, skipping.";
            continue;
        }
        const std::string& component_name = config->component_name();

        try {
            // Pass the relevant subtree to the component config
            config->from_ptree(config_tree_.get_child(component_name));
            config->validate();
            SHIELD_LOG_DEBUG << "Loaded configuration for component: "
                             << component_name;
        } catch (const boost::property_tree::ptree_bad_path& e) {
            SHIELD_LOG_WARN
                << "No configuration found for component: " << component_name
                << ", using defaults. Error: " << e.what();
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Failed to load configuration for component "
                             << component_name << ": " << e.what();
            throw;
        }
    }
}

void ConfigManager::reload_config(const std::string& config_file,
                                  ConfigFormat format) {
    SHIELD_LOG_INFO << "Reloading config file: " << config_file;

    try {
        switch (format) {
            case ConfigFormat::YAML: {
                YAML::Node yaml_node = YAML::LoadFile(config_file);
                config_tree_ = yaml_to_ptree(yaml_node);
                break;
            }
            case ConfigFormat::JSON: {
                std::ifstream ifs(config_file);
                boost::property_tree::read_json(ifs, config_tree_);
                break;
            }
            case ConfigFormat::INI: {
                std::ifstream ifs(config_file);
                boost::property_tree::read_ini(ifs, config_tree_);
                break;
            }
        }
        load_component_configs(true);  // Re-apply configurations to components
        SHIELD_LOG_INFO << "Successfully reloaded config file: " << config_file;
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to reload config file: " << config_file
                         << ", Error: " << e.what();
        throw std::runtime_error("Failed to reload config file: " +
                                 config_file + ", Error: " + e.what());
    }
}

}  // namespace shield::config