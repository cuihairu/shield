#pragma once
#include "shield/core/command.hpp"

namespace shield::commands {

class MigrateCommand : public shield::core::Command {
public:
    MigrateCommand();
    int run(shield::core::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands