#include "shield/core/command_line_parser.hpp"
#include <iostream>
#include "shield/version.hpp"
#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace shield::core {

CommandLineOptions CommandLineParser::parse(int argc, char* argv[]) {
    CommandLineOptions options;
    try {
        po::options_description desc("Allowed options");
        desc.add_options()
            ("version,v", "Print version information")
            ("config,c", po::value<std::string>(), "Specify configuration file")
            ("test,t", po::value<std::string>(), "Test configuration file")
            ("help,h", "Print help information")
        ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            options.show_help = true;
            std::cout << desc << std::endl;
        }

        if (vm.count("version")) {
            options.show_version = true;
            std::cout << "Shield Game Framework v" << shield::VERSION << " (Git Commit: " << GIT_COMMIT_HASH << ")" << std::endl;
        }

        if (vm.count("config")) {
            options.config_file = vm["config"].as<std::string>();
        }
        if (vm.count("test")) {
            options.config_file = vm["test"].as<std::string>();
            options.test = true;
        }
    }
    catch (const po::error& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        options.show_help = true; // Show help on error
    }

    return options;
}

} // namespace shield::core
