#pragma once
#include <memory>

#include "shield/cli/command.hpp"

namespace shield::cli {

// Root command that manages all subcommands
class RootCommand : public Command,
                    public std::enable_shared_from_this<RootCommand> {
public:
    static std::shared_ptr<RootCommand> create();

    int run(CommandContext& ctx) override;

private:
    RootCommand();
    void register_commands();
};

// Command factory for easy registration
class CommandRegistry {
public:
    static std::shared_ptr<RootCommand> create_root_command();
};

}  // namespace shield::cli