#include "shield/core/root_command.hpp"
#include <iostream>
#include <csignal>

int main(int argc, char* argv[]) {
    try {
        auto root_cmd = shield::core::CommandRegistry::create_root_command();
        return root_cmd->execute(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
