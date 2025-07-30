#include "shield/core/config.hpp"

#include <filesystem>
#include <stdexcept>

#include "shield/core/config_def.hpp"
#include "shield/core/config_serialization.hpp"
#include "shield/log/logger.hpp"  // Include logger for debug output

namespace shield::core {

std::string to_yaml_string(const config::ShieldConfig& config) {
    YAML::Node node = YAML::convert<config::ShieldConfig>::encode(config);
    std::stringstream ss;
    ss << node;
    return ss.str();
}

void Config::load(const std::string& file_path) {
    SHIELD_LOG_INFO << "Attempting to load config file: " << file_path;
    try {
        config_ = YAML::LoadFile(file_path);
        SHIELD_LOG_INFO << "Successfully loaded config file: " << file_path;
    } catch (const YAML::Exception& e) {
        SHIELD_LOG_ERROR << "Failed to load config file: " << file_path
                         << ", Error: " << e.what();
        throw std::runtime_error("Failed to load config file: " + file_path +
                                 ", Error: " + e.what());
    }
}

void Config::load_from_string(const std::string& yaml_content) {
    SHIELD_LOG_INFO << "Attempting to load config from string.";
    try {
        config_ = YAML::Load(yaml_content);
        SHIELD_LOG_INFO << "Successfully loaded config from string.";
    } catch (const YAML::Exception& e) {
        SHIELD_LOG_ERROR << "Failed to load config from string. Error: "
                         << e.what();
        throw std::runtime_error("Failed to load config from string. Error: " +
                                 std::string(e.what()));
    }
}

void Config::reset() {
    config_ = YAML::Node();  // Reset to an empty node
    SHIELD_LOG_INFO << "Config singleton reset.";
}

// Helper function to merge YAML nodes (simplified version)
YAML::Node merge_yaml_nodes(const YAML::Node& base,
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

void Config::load_with_profile(const std::string& profile) {
    YAML::Node base_config;

    // 1. Load base config (app.yaml) - 必须存在
    try {
        base_config = YAML::LoadFile(ConfigPaths::DEFAULT_CONFIG_FILE);
        SHIELD_LOG_INFO << "Loaded base configuration: "
                        << ConfigPaths::DEFAULT_CONFIG_FILE;
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to load base config: " +
                                 std::string(e.what()));
    }

    // 2. Load profile-specific config if specified
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

    config_ = base_config;
    SHIELD_LOG_INFO << "Configuration loaded successfully"
                    << (profile.empty() ? "" : " with profile: " + profile);
}

// Helper function to convert YAML node to sol2 object (TODO: implement when
// needed)
/*
sol::object yaml_to_sol(sol::state& lua, const YAML::Node& node) {
    // Implementation commented out for now
    return sol::nil;
}

void Config::bind_to_lua(sol::state& lua) {
    // Implementation commented out for now
}

void Config::bind_section_to_lua(sol::state& lua, const std::string&
section_key, const std::string& lua_table_name) {
    // Implementation commented out for now
}
*/

}  // namespace shield::core
