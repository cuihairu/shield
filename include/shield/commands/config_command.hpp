#pragma once
#include "shield/cli/command.hpp"

namespace shield::commands {

class ConfigCommand : public shield::cli::Command {
public:
    ConfigCommand();
    int run(shield::cli::CommandContext& ctx) override;

private:
    void setup_flags();
    int handle_init(const std::string& directory);
    std::string generate_default_config();

    // Configuration operation methods
    int validate_config(const std::string& config_file);
    int dump_config(const std::string& config_file);
    int get_config_value(const std::string& config_file,
                         const std::string& key);
};

}  // namespace shield::commands