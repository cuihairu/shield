#pragma once
#include "shield/core/command.hpp"

namespace shield::commands {

class CLICommand : public shield::core::Command {
public:
    CLICommand();
    int run(shield::core::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands