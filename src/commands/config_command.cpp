#include "shield/commands/config_command.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "shield/config/config.hpp"

namespace shield::commands {

ConfigCommand::ConfigCommand()
    : shield::cli::Command("config", "Configuration management tool") {
    setup_flags();
    set_long_description("Manage Shield server configuration files.")
        .set_usage("shield config [OPTIONS]")
        .set_example(
            "  shield config --validate\\n"
            "  shield config --dump\\n"
            "  shield config --generate > default_config.yaml");
}

void ConfigCommand::setup_flags() {
    add_bool_flag("validate", "Validate configuration file", false);
    add_bool_flag("dump", "Dump current configuration", false);
    add_bool_flag("generate", "Generate default configuration", false);
    add_flag("config", "Configuration file path", "config/shield.yaml");
    add_flag("get", "Get specific configuration value", "");
}

int ConfigCommand::run(shield::cli::CommandContext& ctx) {
    std::string config_file = ctx.get_flag("config");

    if (ctx.get_bool_flag("generate")) {
        std::cout << generate_default_config() << std::endl;
        return 0;
    }

    if (ctx.get_bool_flag("validate")) {
        return validate_config(config_file);
    }

    if (ctx.get_bool_flag("dump")) {
        return dump_config(config_file);
    }

    if (!ctx.get_flag("get").empty()) {
        return get_config_value(config_file, ctx.get_flag("get"));
    }

    // Show usage information
    std::cout << long_description() << std::endl;
    return 0;
}

std::string ConfigCommand::generate_default_config() {
    std::stringstream ss;
    // Get current time
    std::time_t now = std::time(nullptr);
    char mbstr[100];
    if (std::strftime(mbstr, sizeof(mbstr), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&now))) {
        ss << "# Generated on: " << mbstr << "\\n";
    }
    ss << "# Shield Game Server Configuration\\n";
    ss << "# This is the main configuration file for Shield framework\\n\\n";

    // Generate a basic default configuration
    ss << "app:\\n";
    ss << "  show_banner: true\\n\\n";
    ss << "log:\\n";
    ss << "  level: info\\n";
    ss << "  file: logs/shield.log\\n\\n";
    ss << "gateway:\\n";
    ss << "  listener:\\n";
    ss << "    host: 0.0.0.0\\n";
    ss << "    port: 8080\\n";
    ss << "  tcp:\\n";
    ss << "    enabled: true\\n";
    ss << "  udp:\\n";
    ss << "    enabled: true\\n\\n";
    ss << "prometheus:\\n";
    ss << "  server:\\n";
    ss << "    enabled: true\\n";
    ss << "    host: 0.0.0.0\\n";
    ss << "    port: 9090\\n";

    return ss.str();
}

int ConfigCommand::validate_config(const std::string& file_path) {
    std::cout << "Validating config file: " << file_path << std::endl;
    try {
        auto& config_manager = shield::config::ConfigManager::instance();
        config_manager.load_config(file_path,
                                   shield::config::ConfigFormat::YAML);
        std::cout << "Configuration is valid." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Configuration validation failed: " << e.what()
                  << std::endl;
        return 1;
    }
}

int ConfigCommand::dump_config(const std::string& file_path) {
    std::cout << "Dumping current configuration..." << std::endl;
    try {
        auto& config_manager = shield::config::ConfigManager::instance();
        config_manager.load_config(file_path,
                                   shield::config::ConfigFormat::YAML);

        // For now just indicate success - full dump would require YAML
        // serialization
        std::cout << "Configuration loaded successfully from: " << file_path
                  << std::endl;
        std::cout << "(Full dump functionality requires YAML serialization - "
                     "not implemented yet)"
                  << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load configuration: " << e.what() << std::endl;
        return 1;
    }
}

int ConfigCommand::get_config_value(const std::string& file_path,
                                    const std::string& key) {
    try {
        auto& config_manager = shield::config::ConfigManager::instance();
        config_manager.load_config(file_path,
                                   shield::config::ConfigFormat::YAML);

        const auto& config_tree = config_manager.get_config_tree();
        try {
            std::string value = config_tree.get<std::string>(key);
            std::cout << value << std::endl;
            return 0;
        } catch (...) {
            std::cerr << "Key not found: " << key << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to load configuration: " << e.what() << std::endl;
        return 1;
    }
}

}  // namespace shield::commands