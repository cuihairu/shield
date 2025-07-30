#pragma once
#include "shield/core/command.hpp"

namespace shield::commands {

class ConfigCommand : public shield::core::Command {
public:
    ConfigCommand();
    int run(shield::core::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands