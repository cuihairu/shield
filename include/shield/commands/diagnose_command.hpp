#pragma once
#include "shield/core/command.hpp"

namespace shield::commands {

class DiagnoseCommand : public shield::core::Command {
public:
    DiagnoseCommand();
    int run(shield::core::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands