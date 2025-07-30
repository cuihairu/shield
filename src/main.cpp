#include <csignal>
#include <iostream>

#include "shield/cli/root_command.hpp"

int main(int argc, char* argv[]) {
    try {
        auto root_cmd = shield::cli::CommandRegistry::create_root_command();
        return root_cmd->execute(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
