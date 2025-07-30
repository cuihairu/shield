#pragma once

#include <yaml-cpp/yaml.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace shield::core {

// Configuration file path constants
namespace ConfigPaths {
constexpr const char* DEFAULT_CONFIG_FILE = "config/app.yaml";

// Generate profile-specific config file path
inline std::string get_profile_config_file(const std::string& profile) {
    return "config/app-" + profile + ".yaml";
}
}  // namespace ConfigPaths

class Config {
public:
    static Config& instance() {
        static Config instance;
        return instance;
    }

    void load(const std::string& file_path);
    void load_from_string(const std::string& yaml_content);
    void load_with_profile(const std::string& profile = "");
    void reset();

    // Lua integration methods using sol2 (TODO: implement when needed)
    // void bind_to_lua(sol::state& lua);
    // void bind_section_to_lua(sol::state& lua, const std::string& section_key,
    // const std::string& lua_table_name = "config");

    // Convenience method to load default config
    void load_default() { load(ConfigPaths::DEFAULT_CONFIG_FILE); }

    // Load configuration for CLI operations
    void load_for_cli() {
        load_default();  // Use default for now
    }

    // Load minimal configuration for diagnostics
    void load_for_diagnose() {
        load_default();  // Use default for now
    }

    // Get config file path for different environments
    static const char* get_default_config_path() {
        return ConfigPaths::DEFAULT_CONFIG_FILE;
    }

    static const char* get_test_config_path() {
        return ConfigPaths::DEFAULT_CONFIG_FILE;  // Use same config for tests
    }

    template <typename T>
    T get(const std::string& key) const {
        try {
            YAML::Node node = YAML::Load(YAML::Dump(config_));
            std::stringstream ss(key);
            std::string segment;
            while (std::getline(ss, segment, '.')) {
                node = node[segment];
            }
            return node.as<T>();
        } catch (const YAML::Exception& e) {
            throw std::runtime_error("Config key not found: " + key + " (" +
                                     e.what() + ")");
        }
    }

private:
    Config() = default;
    YAML::Node config_;
};

namespace config {
struct ShieldConfig;
}

std::string to_yaml_string(const config::ShieldConfig& config);

}  // namespace shield::core