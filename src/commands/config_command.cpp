#include "shield/commands/config_command.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "shield/config/config.hpp"
#include "shield/config/config_def.hpp"

namespace shield::commands {

ConfigCommand::ConfigCommand()
    : shield::cli::Command("config", "Configuration management tool") {
    setup_flags();
    set_long_description("Manage Shield server configuration files.")
        .set_usage("shield config [OPTIONS]")
        .set_example(
            "  shield config --validate\\n"
            "  shield config --dump\\n"
            "  shield config --get logger.level\\n"
            "  shield config --file config/prod.yaml --validate\\n"
            "  shield config --init\\n"
            "  shield config --init /path/to/project");
}

void ConfigCommand::setup_flags() {
    add_bool_flag("validate", "Validate configuration file", false);
    add_bool_flag("dump", "Dump current configuration", false);
    add_flag("get", "Get configuration value", "");
    add_flag_with_short("file", "f", "Specify configuration file",
                        "config/app.yaml");
    add_flag("init", "Initialize configuration in directory (default: current)",
             ".");
}

int ConfigCommand::run(shield::cli::CommandContext& ctx) {
    std::cout << "Shield Configuration Management" << std::endl;

    // Handle init command first - check if user provided the flag
    if (ctx.is_user_provided("init")) {
        std::string init_dir = ctx.get_flag("init");
        return handle_init(init_dir);
    }

    std::string config_file = ctx.get_flag("file");
    std::cout << "Using config file: " << config_file << std::endl;

    if (ctx.get_bool_flag("validate")) {
        return validate_config(config_file);
    }

    if (ctx.get_bool_flag("dump")) {
        return dump_config(config_file);
    }

    std::string get_key = ctx.get_flag("get");
    if (!get_key.empty()) {
        return get_config_value(config_file, get_key);
    }

    print_help();
    return 0;
}

int ConfigCommand::handle_init(const std::string& directory) {
    std::cout << "Initializing Shield configuration in: " << directory
              << std::endl;

    try {
        // Create directory if it doesn't exist
        std::filesystem::path dir_path(directory);
        if (!std::filesystem::exists(dir_path)) {
            std::filesystem::create_directories(dir_path);
            std::cout << "Created directory: " << directory << std::endl;
        }

        // Create config subdirectory
        std::filesystem::path config_dir = dir_path / "config";
        if (!std::filesystem::exists(config_dir)) {
            std::filesystem::create_directories(config_dir);
            std::cout << "Created config directory: " << config_dir
                      << std::endl;
        }

        // Create default configuration file
        std::filesystem::path config_file = config_dir / "app.yaml";
        if (std::filesystem::exists(config_file)) {
            std::cout << "Configuration file already exists: " << config_file
                      << std::endl;
            std::cout << "Use --force to overwrite existing configuration."
                      << std::endl;
            return 1;
        }

        // Write default configuration
        std::ofstream file(config_file);
        if (!file.is_open()) {
            std::cerr << "Failed to create configuration file: " << config_file
                      << std::endl;
            return 1;
        }

        file << generate_default_config();
        file.close();

        std::cout << "✓ Created default configuration: " << config_file
                  << std::endl;
        std::cout << "✓ Configuration initialization completed successfully!"
                  << std::endl;
        std::cout << std::endl;
        std::cout << "Next steps:" << std::endl;
        std::cout << "  1. Review and modify " << config_file << std::endl;
        std::cout << "  2. Validate with: shield config --file " << config_file
                  << " --validate" << std::endl;
        std::cout << "  3. Start server: shield server --config " << config_file
                  << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error initializing configuration: " << e.what()
                  << std::endl;
        return 1;
    }
}

std::string ConfigCommand::generate_default_config() {
    shield::config::config::ShieldConfig config =
        shield::config::get_default_shield_config();
    std::stringstream ss;
    // Get current time
    std::time_t now = std::time(nullptr);
    char mbstr[100];
    if (std::strftime(mbstr, sizeof(mbstr), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&now))) {
        ss << "# Generated on: " << mbstr << "\n";
    }
    ss << "# Shield Game Server Configuration\n";
    ss << "# This is the main configuration file for Shield framework\n\n";
    ss << shield::config::to_yaml_string(config);

    return ss.str();
}

int ConfigCommand::validate_config(const std::string& file_path) {
    std::cout << "Validating config file: " << file_path << std::endl;
    try {
        shield::config::Config::instance().load(file_path);
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
        shield::config::Config::instance().load(file_path);
        // Assuming Config::instance().config_ is accessible or there's a method
        // to get the YAML::Node For now, we'll just print a success message.
        std::cout << "(Dump functionality not fully implemented yet, but "
                     "config loaded successfully)"
                  << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Failed to dump configuration: " << e.what() << std::endl;
        return 1;
    }
}

int ConfigCommand::get_config_value(const std::string& file_path,
                                    const std::string& key) {
    std::cout << "Getting value for key '" << key
              << "' from file: " << file_path << std::endl;
    try {
        shield::config::Config::instance().load(file_path);
        // This will throw if the key is not found, which is handled by the
        // catch block
        std::string value =
            shield::config::Config::instance().get<std::string>(key);
        std::cout << "Value for '" << key << "': " << value << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Failed to get config value: " << e.what() << std::endl;
        return 1;
    }
}

}  // namespace shield::commands