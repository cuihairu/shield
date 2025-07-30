#pragma once
#include "shield/cli/command.hpp"

namespace shield::commands {

class MigrateCommand : public shield::cli::Command {
public:
    MigrateCommand();
    int run(shield::cli::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands