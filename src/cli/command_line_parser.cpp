#include "shield/cli/command_line_parser.hpp"

#include <algorithm>
#include <boost/program_options.hpp>
#include <iostream>

#include "shield/version.hpp"

namespace po = boost::program_options;

namespace shield::cli {

CommandLineOptions CommandLineParser::parse(int argc, char* argv[]) {
    CommandLineOptions options;

    if (argc < 2) {
        // No subcommand, default behavior (server mode)
        options.subcommand = SubCommand::Server;
        return options;
    }

    try {
        // Check for global options first
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--version" || arg == "-v") {
                options.show_version = true;
                return options;
            } else if (arg == "--help" || arg == "-h") {
                options.show_help = true;
                return options;
            } else if (arg == "--config" || arg == "-c") {
                if (i + 1 < argc) {
                    options.config_file = argv[++i];
                }
            } else if (arg[0] != '-') {
                // This is likely a subcommand
                options.subcommand = parse_subcommand(arg);

                // Parse subcommand-specific options
                int remaining_argc = argc - i;
                char** remaining_argv = argv + i;

                switch (options.subcommand) {
                    case SubCommand::Server:
                        parse_server_options(options, remaining_argc,
                                             remaining_argv, 1);
                        break;
                    case SubCommand::CLI:
                        parse_cli_options(options, remaining_argc,
                                          remaining_argv, 1);
                        break;
                    case SubCommand::Migrate:
                        parse_migrate_options(options, remaining_argc,
                                              remaining_argv, 1);
                        break;
                    case SubCommand::Test:
                        parse_test_options(options, remaining_argc,
                                           remaining_argv, 1);
                        break;
                    case SubCommand::Config:
                        parse_config_options(options, remaining_argc,
                                             remaining_argv, 1);
                        break;
                    default:
                        std::cerr << "Unknown subcommand: " << arg << std::endl;
                        options.show_help = true;
                        break;
                }
                break;
            }
        }

        // If no subcommand found, default to server
        if (options.subcommand == SubCommand::None) {
            options.subcommand = SubCommand::Server;
        }

    } catch (const po::error& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        options.show_help = true;
    }

    return options;
}

SubCommand CommandLineParser::parse_subcommand(const std::string& cmd) {
    if (cmd == "server") return SubCommand::Server;
    if (cmd == "cli") return SubCommand::CLI;
    if (cmd == "migrate") return SubCommand::Migrate;
    if (cmd == "test") return SubCommand::Test;
    if (cmd == "config") return SubCommand::Config;
    return SubCommand::None;
}

void CommandLineParser::parse_server_options(CommandLineOptions& options,
                                             int argc, char* argv[],
                                             int start_idx) {
    try {
        po::options_description desc("Server options");
        desc.add_options()("config,c", po::value<std::string>(),
                           "Configuration file")("port,p", po::value<int>(),
                                                 "Server port")(
            "host", po::value<std::string>(), "Server host")(
            "daemon,d", "Run as daemon")("help,h", "Show server help");

        po::variables_map vm;
        po::store(
            po::parse_command_line(argc - start_idx, argv + start_idx, desc),
            vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << "Shield Server - Game server daemon\n\n"
                      << desc << std::endl;
            options.show_help = true;
            return;
        }

        if (vm.count("config")) {
            options.config_file = vm["config"].as<std::string>();
        }
        if (vm.count("port")) {
            options.subcommand_args["port"] =
                std::to_string(vm["port"].as<int>());
        }
        if (vm.count("host")) {
            options.subcommand_args["host"] = vm["host"].as<std::string>();
        }
        if (vm.count("daemon")) {
            options.subcommand_args["daemon"] = "true";
        }
    } catch (const po::error& e) {
        std::cerr << "Error parsing server options: " << e.what() << std::endl;
        options.show_help = true;
    }
}

void CommandLineParser::parse_cli_options(CommandLineOptions& options, int argc,
                                          char* argv[], int start_idx) {
    try {
        po::options_description desc("CLI options");
        desc.add_options()("url,u", po::value<std::string>(), "Server URL")(
            "timeout,t", po::value<int>(), "Request timeout in seconds")(
            "format,f", po::value<std::string>(),
            "Output format (json, yaml, table)")("verbose,v", "Verbose output")(
            "help,h", "Show CLI help");

        po::variables_map vm;
        po::store(
            po::parse_command_line(argc - start_idx, argv + start_idx, desc),
            vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout
                << "Shield CLI - Command line interface for Shield server\n\n"
                << desc << std::endl;
            options.show_help = true;
            return;
        }

        if (vm.count("url")) {
            options.subcommand_args["url"] = vm["url"].as<std::string>();
        }
        if (vm.count("timeout")) {
            options.subcommand_args["timeout"] =
                std::to_string(vm["timeout"].as<int>());
        }
        if (vm.count("format")) {
            options.subcommand_args["format"] = vm["format"].as<std::string>();
        }
        if (vm.count("verbose")) {
            options.subcommand_args["verbose"] = "true";
        }
    } catch (const po::error& e) {
        std::cerr << "Error parsing CLI options: " << e.what() << std::endl;
        options.show_help = true;
    }
}

void CommandLineParser::parse_migrate_options(CommandLineOptions& options,
                                              int argc, char* argv[],
                                              int start_idx) {
    try {
        po::options_description desc("Migration options");
        desc.add_options()("from", po::value<std::string>(), "Source version")(
            "to", po::value<std::string>(), "Target version")(
            "dry-run", "Show what would be migrated without making changes")(
            "backup", "Create backup before migration")("help,h",
                                                        "Show migration help");

        po::variables_map vm;
        po::store(
            po::parse_command_line(argc - start_idx, argv + start_idx, desc),
            vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << "Shield Migrate - Database and configuration "
                         "migration tool\n\n"
                      << desc << std::endl;
            options.show_help = true;
            return;
        }

        if (vm.count("from")) {
            options.subcommand_args["from"] = vm["from"].as<std::string>();
        }
        if (vm.count("to")) {
            options.subcommand_args["to"] = vm["to"].as<std::string>();
        }
        if (vm.count("dry-run")) {
            options.subcommand_args["dry-run"] = "true";
        }
        if (vm.count("backup")) {
            options.subcommand_args["backup"] = "true";
        }
    } catch (const po::error& e) {
        std::cerr << "Error parsing migration options: " << e.what()
                  << std::endl;
        options.show_help = true;
    }
}

void CommandLineParser::parse_test_options(CommandLineOptions& options,
                                           int argc, char* argv[],
                                           int start_idx) {
    try {
        po::options_description desc("Test options");
        desc.add_options()("suite,s", po::value<std::string>(),
                           "Test suite to run (unit, integration, e2e)")(
            "filter,f", po::value<std::string>(), "Test filter pattern")(
            "config,c", po::value<std::string>(), "Test configuration file")(
            "parallel,j", po::value<int>(), "Number of parallel test threads")(
            "help,h", "Show test help");

        po::variables_map vm;
        po::store(
            po::parse_command_line(argc - start_idx, argv + start_idx, desc),
            vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << "Shield Test - Test runner for Shield framework\n\n"
                      << desc << std::endl;
            options.show_help = true;
            return;
        }

        if (vm.count("suite")) {
            options.subcommand_args["suite"] = vm["suite"].as<std::string>();
        }
        if (vm.count("filter")) {
            options.subcommand_args["filter"] = vm["filter"].as<std::string>();
        }
        if (vm.count("config")) {
            options.subcommand_args["test-config"] =
                vm["config"].as<std::string>();
        }
        if (vm.count("parallel")) {
            options.subcommand_args["parallel"] =
                std::to_string(vm["parallel"].as<int>());
        }
    } catch (const po::error& e) {
        std::cerr << "Error parsing test options: " << e.what() << std::endl;
        options.show_help = true;
    }
}

void CommandLineParser::parse_config_options(CommandLineOptions& options,
                                             int argc, char* argv[],
                                             int start_idx) {
    try {
        po::options_description desc("Configuration options");
        desc.add_options()("validate", "Validate configuration file")(
            "dump", "Dump current configuration")(
            "set", po::value<std::string>(),
            "Set configuration value (key=value)")(
            "get", po::value<std::string>(), "Get configuration value")(
            "help,h", "Show config help");

        po::variables_map vm;
        po::store(
            po::parse_command_line(argc - start_idx, argv + start_idx, desc),
            vm);
        po::notify(vm);

        if (vm.count("help")) {
            std::cout << "Shield Config - Configuration management tool\n\n"
                      << desc << std::endl;
            options.show_help = true;
            return;
        }

        if (vm.count("validate")) {
            options.subcommand_args["validate"] = "true";
        }
        if (vm.count("dump")) {
            options.subcommand_args["dump"] = "true";
        }
        if (vm.count("set")) {
            options.subcommand_args["set"] = vm["set"].as<std::string>();
        }
        if (vm.count("get")) {
            options.subcommand_args["get"] = vm["get"].as<std::string>();
        }
    } catch (const po::error& e) {
        std::cerr << "Error parsing config options: " << e.what() << std::endl;
        options.show_help = true;
    }
}

void CommandLineParser::show_help(SubCommand cmd) {
    std::cout << "Shield Game Framework v" << shield::VERSION << "\n\n";

    if (cmd == SubCommand::None) {
        std::cout << "Usage: shield [GLOBAL_OPTIONS] <SUBCOMMAND> "
                     "[SUBCOMMAND_OPTIONS]\n\n";
        std::cout << "Available subcommands:\n";
        std::cout << "  server    Start the Shield game server\n";
        std::cout
            << "  cli       Command line interface for server management\n";
        std::cout << "  migrate   Database and configuration migration tools\n";
        std::cout << "  test      Run tests\n";
        std::cout << "  config    Configuration management\n\n";
        std::cout << "Global options:\n";
        std::cout << "  --version, -v     Show version information\n";
        std::cout << "  --help, -h        Show this help message\n";
        std::cout << "  --config, -c      Specify configuration file\n\n";
        std::cout << "Use 'shield <subcommand> --help' for subcommand-specific "
                     "help.\n";
    }
}

}  // namespace shield::cli
