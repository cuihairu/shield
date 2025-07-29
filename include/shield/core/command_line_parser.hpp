#pragma once
#include <string>

namespace shield::core {

struct CommandLineOptions {
    bool show_version = false;
    bool show_help = false;
    bool test = false;
    std::string config_file;
};

class CommandLineParser {
public:
    static CommandLineOptions parse(int argc, char* argv[]);
};

} // namespace shield::core