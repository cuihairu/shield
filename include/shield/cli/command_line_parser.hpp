#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace shield::cli {

// Subcommand types
enum class SubCommand {
    None,
    Server,    // shield server --config config.yaml
    CLI,       // shield cli --url http://localhost:8080
    Migrate,   // shield migrate --from v1.0 --to v2.0
    Test,      // shield test --suite integration
    Config     // shield config --validate
};

struct CommandLineOptions {
    // Global options
    bool show_version = false;
    bool show_help = false;
    std::string config_file;
    
    // Subcommand
    SubCommand subcommand = SubCommand::None;
    
    // Subcommand-specific options
    std::unordered_map<std::string, std::string> subcommand_args;
    std::vector<std::string> positional_args;
};

class CommandLineParser {
public:
    static CommandLineOptions parse(int argc, char *argv[]);
    
private:
    static SubCommand parse_subcommand(const std::string& cmd);
    static void parse_server_options(CommandLineOptions& options, int argc, char* argv[], int start_idx);
    static void parse_cli_options(CommandLineOptions& options, int argc, char* argv[], int start_idx);
    static void parse_migrate_options(CommandLineOptions& options, int argc, char* argv[], int start_idx);
    static void parse_test_options(CommandLineOptions& options, int argc, char* argv[], int start_idx);
    static void parse_config_options(CommandLineOptions& options, int argc, char* argv[], int start_idx);
    static void show_help(SubCommand cmd = SubCommand::None);
};

}  // namespace shield::core