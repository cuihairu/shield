#pragma once
#include "shield/cli/command.hpp"

namespace shield::commands {

class CLICommand : public shield::cli::Command {
public:
    CLICommand();
    int run(shield::cli::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands