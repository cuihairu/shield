#pragma once
#include "shield/cli/command.hpp"

namespace shield::commands {

class DiagnoseCommand : public shield::cli::Command {
public:
    DiagnoseCommand();
    int run(shield::cli::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands