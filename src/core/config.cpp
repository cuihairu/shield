#include "shield/core/config.hpp"
#include <stdexcept>
#include "shield/core/logger.hpp" // Include logger for debug output

namespace shield::core {

void Config::load(const std::string& file_path) {
    SHIELD_LOG_INFO << "Attempting to load config file: " << file_path;
    try {
        config_ = YAML::LoadFile(file_path);
        SHIELD_LOG_INFO << "Successfully loaded config file: " << file_path;
    } catch (const YAML::Exception& e) {
        SHIELD_LOG_ERROR << "Failed to load config file: " << file_path << ", Error: " << e.what();
        throw std::runtime_error("Failed to load config file: " + file_path + ", Error: " + e.what());
    }
}

void Config::load_from_string(const std::string& yaml_content) {
    SHIELD_LOG_INFO << "Attempting to load config from string.";
    try {
        config_ = YAML::Load(yaml_content);
        SHIELD_LOG_INFO << "Successfully loaded config from string.";
    } catch (const YAML::Exception& e) {
        SHIELD_LOG_ERROR << "Failed to load config from string. Error: " << e.what();
        throw std::runtime_error("Failed to load config from string. Error: " + std::string(e.what()));
    }
}

void Config::reset() {
    config_ = YAML::Node(); // Reset to an empty node
    SHIELD_LOG_INFO << "Config singleton reset.";
}

} // namespace shield::core 
