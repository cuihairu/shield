#pragma once
#include "shield/core/command.hpp"

namespace shield::commands {

class ServerCommand : public shield::core::Command {
public:
    ServerCommand();
    int run(shield::core::CommandContext& ctx) override;

private:
    void setup_flags();
};

}  // namespace shield::commands