#include "shield/core/root_command.hpp"

#include <iostream>

#include "shield/commands/all_commands.hpp"
#include "shield/core/config.hpp"
#include "shield/version.hpp"

namespace shield::core {

RootCommand::RootCommand()
    : Command("shield",
              "Shield Game Framework - A high-performance game server") {
    set_long_description(
        "Shield is a modern game server framework built with C++20. "
        "It provides high-performance networking, scripting, and "
        "distributed system capabilities for multiplayer games.")
        .set_usage("shield [GLOBAL_OPTIONS] <COMMAND> [COMMAND_OPTIONS]")
        .set_example(
            "  shield server --config config/prod.yaml\\n"
            "  shield cli --url http://localhost:8080\\n"
            "  shield config --validate");

    // Add global flags
    add_flag_with_short("config", "c", "Global configuration file",
                        ConfigPaths::DEFAULT_CONFIG_FILE);
    add_bool_flag_with_short("version", "v", "Show version information", false);

    register_commands();
}

std::shared_ptr<RootCommand> RootCommand::create() {
    // Use make_shared with a helper struct to access private constructor
    struct MakeSharedEnabler : public RootCommand {};
    auto root = std::make_shared<MakeSharedEnabler>();
    return std::static_pointer_cast<RootCommand>(root);
}

void RootCommand::register_commands() {
    // Register all subcommands
    add_command(std::make_shared<shield::commands::ServerCommand>());
    add_command(std::make_shared<shield::commands::CLICommand>());
    add_command(std::make_shared<shield::commands::ConfigCommand>());
    add_command(std::make_shared<shield::commands::DiagnoseCommand>());
    add_command(std::make_shared<shield::commands::MigrateCommand>());
}

int RootCommand::run(CommandContext& ctx) {
    // Handle global flags
    if (ctx.get_bool_flag("version")) {
        shield::print_version();
        return 0;
    }

    // If no subcommand specified, default to server
    std::cout << "No subcommand specified. Starting server..." << std::endl;

    // Create and run server command with current context
    auto server_cmd = std::make_shared<shield::commands::ServerCommand>();
    return server_cmd->run(ctx);
}

// CommandRegistry implementation
std::shared_ptr<RootCommand> CommandRegistry::create_root_command() {
    return RootCommand::create();
}

}  // namespace shield::core