#include "shield/commands/cli_command.hpp"

#include <iostream>

#include "shield/config/config.hpp"

namespace shield::commands {

CLICommand::CLICommand()
    : shield::cli::Command(
          "cli", "Command line interface for Shield server management") {
    setup_flags();
    set_long_description(
        "Connect to a running Shield server and execute management commands.")
        .set_usage("shield cli [OPTIONS] [COMMAND]")
        .set_example(
            "  shield cli --url http://localhost:8080\\n"
            "  shield cli --url remote.server.com --timeout 30");
}

void CLICommand::setup_flags() {
    add_flag("url", "Server URL", "http://localhost:8080");
    add_int_flag("timeout", "Request timeout in seconds", 30);
    add_flag("format", "Output format (json, yaml, table)", "table");
    add_bool_flag("verbose", "Verbose output", false);
}

int CLICommand::run(shield::cli::CommandContext& ctx) {
    // Load CLI-specific layered configuration
    auto& config = shield::config::Config::instance();
    try {
        config.load_for_cli();
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to load CLI configuration: " << e.what()
                  << std::endl;
        // Continue with defaults
    }

    std::cout << "Shield CLI Client" << std::endl;

    // Use configuration with command line overrides
    std::string server_url = ctx.get_flag("url");
    if (server_url.empty() || server_url == "http://localhost:8080") {
        try {
            server_url = config.get<std::string>("client.default_url");
        } catch (...) {
            server_url = "http://localhost:8080";  // fallback
        }
    }

    int timeout = ctx.get_int_flag("timeout");
    if (timeout == 30) {  // default value
        try {
            timeout = config.get<int>("client.timeout");
        } catch (...) {
            timeout = 30;  // fallback
        }
    }

    std::cout << "Connecting to: " << server_url << std::endl;
    std::cout << "Timeout: " << timeout << "s" << std::endl;
    std::cout << "Format: " << ctx.get_flag("format") << std::endl;

    if (ctx.get_bool_flag("verbose")) {
        std::cout << "Verbose mode enabled" << std::endl;
    }

    // TODO: Implement CLI client logic
    std::cout << "CLI functionality would be implemented here" << std::endl;
    return 0;
}

}  // namespace shield::commands