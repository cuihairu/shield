#include "shield/config/config.hpp"

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <filesystem>
#include <fstream>

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

    // Load base configuration
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

    // If profile is specified, load profile configuration and merge
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
    // Load configuration data for all registered configuration properties
    for (auto& [type_id, config] : configs_) {
        if (is_reload && !config->supports_hot_reload()) {
            SHIELD_LOG_DEBUG << "Properties " << config->properties_name()
                             << " does not support hot reload, skipping.";
            continue;
        }
        const std::string& properties_name = config->properties_name();

        try {
            // Pass the relevant subtree to the configuration properties
            config->from_ptree(config_tree_.get_child(properties_name));
            config->validate();
            SHIELD_LOG_DEBUG << "Loaded configuration for properties: "
                             << properties_name;
        } catch (const boost::property_tree::ptree_bad_path& e) {
            SHIELD_LOG_WARN
                << "No configuration found for properties: " << properties_name
                << ", using defaults. Error: " << e.what();
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Failed to load configuration for properties "
                             << properties_name << ": " << e.what();
            throw;
        }
    }
}

void ConfigManager::reload_config(const std::string& config_file,
                                  ConfigFormat format) {
    SHIELD_LOG_INFO << "Attempting to reload config from: " << config_file;

    // 1. Load new config into a temporary ptree
    boost::property_tree::ptree new_config_tree;
    try {
        switch (format) {
            case ConfigFormat::YAML: {
                YAML::Node yaml_node = YAML::LoadFile(config_file);
                new_config_tree = yaml_to_ptree(yaml_node);
                break;
            }
            case ConfigFormat::JSON: {
                std::ifstream ifs(config_file);
                boost::property_tree::read_json(ifs, new_config_tree);
                break;
            }
            case ConfigFormat::INI: {
                std::ifstream ifs(config_file);
                boost::property_tree::read_ini(ifs, new_config_tree);
                break;
            }
        }
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to parse new config file, aborting reload: "
                         << e.what();
        return;  // Abort, old config remains active
    }

    // 2. Clone, populate, and validate in a 'sandbox'
    std::unordered_map<std::type_index,
                       std::shared_ptr<ConfigurationProperties>>
        validated_new_configs;

    try {
        std::lock_guard<std::mutex> lock(config_mutex_);
        for (auto const& [type_id, current_config] : configs_) {
            if (!current_config->supports_hot_reload()) {
                continue;  // Skip non-reloadable configs
            }

            auto new_config_clone = current_config->clone();
            const auto& properties_name = new_config_clone->properties_name();

            // Populate from the new ptree
            new_config_clone->from_ptree(
                new_config_tree.get_child(properties_name));

            // Validate the new data
            new_config_clone->validate();

            // If validation passes, add to the staging map
            validated_new_configs[type_id] = std::move(new_config_clone);
        }
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR
            << "Failed to validate new configuration, aborting reload: "
            << e.what();
        return;  // Abort, old config remains active
    }

    // 3. Atomic Swap: If all validations passed, apply the new configs
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_tree_ = new_config_tree;
        for (auto const& [type_id, new_config] : validated_new_configs) {
            configs_[type_id] = new_config;
            config_by_name_[new_config->properties_name()] = new_config;
        }
        SHIELD_LOG_INFO << "Successfully applied new configuration.";
    }

    // 4. Notify Subscribers (outside the config lock)
    std::vector<std::pair<ReloadCallback,
                          std::shared_ptr<const ConfigurationProperties>>>
        callbacks_to_run;
    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        for (const auto& [type_id, new_config] : validated_new_configs) {
            auto it = reload_subscribers_.find(type_id);
            if (it != reload_subscribers_.end()) {
                for (const auto& callback : it->second) {
                    callbacks_to_run.push_back({callback, new_config});
                }
            }
        }
    }

    for (const auto& [callback, config_ptr] : callbacks_to_run) {
        try {
            callback(*config_ptr);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR
                << "Exception in config reload callback for properties '"
                << config_ptr->properties_name() << "': " << e.what();
        }
    }
}

}  // namespace shield::config