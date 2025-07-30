#include "shield/cli/command.hpp"

#include <algorithm>
#include <boost/program_options.hpp>
#include <iomanip>
#include <iostream>

namespace po = boost::program_options;

namespace shield::cli {

Command::Command(const std::string& name, const std::string& description)
    : name_(name), description_(description) {}

void Command::add_command(std::shared_ptr<Command> cmd) {
    cmd->parent_ = this;
    subcommands_.push_back(cmd);
}

std::shared_ptr<Command> Command::find_command(const std::string& name) {
    auto it = std::find_if(subcommands_.begin(), subcommands_.end(),
                           [&name](const std::shared_ptr<Command>& cmd) {
                               return cmd->name() == name;
                           });
    return it != subcommands_.end() ? *it : nullptr;
}

void Command::add_flag(const std::string& name, const std::string& description,
                       const std::string& default_value) {
    flags_.push_back({name, "", description, default_value, "string"});
}

void Command::add_flag_with_short(const std::string& name,
                                  const std::string& short_name,
                                  const std::string& description,
                                  const std::string& default_value) {
    flags_.push_back({name, short_name, description, default_value, "string"});
}

void Command::add_bool_flag(const std::string& name,
                            const std::string& description,
                            bool default_value) {
    flags_.push_back(
        {name, "", description, default_value ? "true" : "false", "bool"});
}

void Command::add_bool_flag_with_short(const std::string& name,
                                       const std::string& short_name,
                                       const std::string& description,
                                       bool default_value) {
    flags_.push_back({name, short_name, description,
                      default_value ? "true" : "false", "bool"});
}

void Command::add_int_flag(const std::string& name,
                           const std::string& description, int default_value) {
    flags_.push_back(
        {name, "", description, std::to_string(default_value), "int"});
}

void Command::add_int_flag_with_short(const std::string& name,
                                      const std::string& short_name,
                                      const std::string& description,
                                      int default_value) {
    flags_.push_back(
        {name, short_name, description, std::to_string(default_value), "int"});
}

int Command::execute(int argc, char* argv[]) {
    try {
        auto cmd = parse_and_execute(argc, argv);
        // Don't check cmd pointer, just return success if no exception
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

std::shared_ptr<Command> Command::parse_and_execute(int argc, char* argv[]) {
    CommandContext ctx;

    // Build program options description
    po::options_description desc("Options");
    desc.add_options()("help,h", "Show help message");

    // Add command-specific flags
    for (const auto& flag : flags_) {
        std::string option_spec = flag.name;
        if (!flag.short_name.empty()) {
            option_spec += "," + flag.short_name;
        }

        if (flag.type == "bool") {
            desc.add_options()(option_spec.c_str(), flag.description.c_str());
        } else if (flag.type == "int") {
            desc.add_options()(
                option_spec.c_str(),
                po::value<int>()->default_value(std::stoi(flag.default_value)),
                flag.description.c_str());
        } else {
            // For string flags, check if they should have implicit values (like --init)
            if (flag.name == "init") {
                desc.add_options()(
                    option_spec.c_str(),
                    po::value<std::string>()->implicit_value(flag.default_value),
                    flag.description.c_str());
            } else {
                desc.add_options()(
                    option_spec.c_str(),
                    po::value<std::string>()->default_value(flag.default_value),
                    flag.description.c_str());
            }
        }
    }

    po::variables_map vm;

    // Parse known args to allow for subcommands
    po::parsed_options parsed = po::command_line_parser(argc, argv)
                                    .options(desc)
                                    .allow_unregistered()
                                    .run();

    po::store(parsed, vm);
    po::notify(vm);

    // Get unrecognized options (potential subcommands and their args)
    std::vector<std::string> unrecognized =
        po::collect_unrecognized(parsed.options, po::include_positional);

    // Check for subcommands BEFORE processing help
    if (!unrecognized.empty() && !subcommands_.empty()) {
        auto subcmd = find_command(unrecognized[0]);
        if (subcmd) {
            // Prepare args for subcommand - include the help flag if present
            std::vector<char*> subcmd_argv;
            subcmd_argv.push_back(argv[0]);  // Program name
            subcmd_argv.push_back(
                const_cast<char*>(unrecognized[0].c_str()));  // Subcommand name

            // Add remaining unrecognized args
            for (size_t i = 1; i < unrecognized.size(); ++i) {
                subcmd_argv.push_back(
                    const_cast<char*>(unrecognized[i].c_str()));
            }

            // Add help flag if it was present in original command
            if (vm.count("help")) {
                subcmd_argv.push_back(const_cast<char*>("-h"));
            }

            return subcmd->parse_and_execute(subcmd_argv.size(),
                                             subcmd_argv.data());
        }
    }

    // Check for help (only for current command if no subcommand found)
    if (vm.count("help")) {
        print_help();
        return nullptr;  // Don't use shared_from_this() here
    }

    // Extract flag values
    for (const auto& flag : flags_) {
        if (vm.count(flag.name)) {
            if (flag.type == "bool") {
                ctx.set_user_flag(flag.name, "true");
            } else if (flag.type == "int") {
                ctx.set_user_flag(flag.name, std::to_string(vm[flag.name].as<int>()));
            } else {
                ctx.set_user_flag(flag.name, vm[flag.name].as<std::string>());
            }
        } else {
            ctx.set_flag(flag.name, flag.default_value);
        }
    }

    // Add positional arguments to context
    for (const auto& arg : unrecognized) {
        ctx.add_arg(arg);
    }

    // Execute this command
    int result = run(ctx);
    if (result != 0) {
        return nullptr;
    }

    return nullptr;  // Don't return shared_from_this()
}

void Command::print_help() const {
    std::cout << name_ << " - " << description_ << std::endl << std::endl;

    if (!long_description_.empty()) {
        std::cout << long_description_ << std::endl << std::endl;
    }

    if (!usage_.empty()) {
        std::cout << "Usage: " << usage_ << std::endl << std::endl;
    } else {
        std::cout << "Usage: " << name_;
        if (!flags_.empty()) {
            std::cout << " [OPTIONS]";
        }
        if (!subcommands_.empty()) {
            std::cout << " <COMMAND>";
        }
        std::cout << std::endl << std::endl;
    }

    if (!subcommands_.empty()) {
        std::cout << "Available Commands:" << std::endl;
        for (const auto& cmd : subcommands_) {
            std::cout << "  " << std::left << std::setw(12) << cmd->name()
                      << cmd->description() << std::endl;
        }
        std::cout << std::endl;
    }

    if (!flags_.empty()) {
        std::cout << "Flags:" << std::endl;
        for (const auto& flag : flags_) {
            std::cout << "  --" << std::left << std::setw(12) << flag.name;
            if (!flag.short_name.empty()) {
                std::cout << "-" << flag.short_name << ", ";
            } else {
                std::cout << "    ";
            }
            std::cout << flag.description;
            if (!flag.default_value.empty()) {
                std::cout << " (default: " << flag.default_value << ")";
            }
            std::cout << std::endl;
        }
        std::cout << std::endl;
    }

    if (!example_.empty()) {
        std::cout << "Examples:" << std::endl;
        // Process escape sequences in examples
        std::string processed_example = example_;
        size_t pos = 0;
        while ((pos = processed_example.find("\\n", pos)) !=
               std::string::npos) {
            processed_example.replace(pos, 2, "\n");
            pos += 1;
        }
        std::cout << processed_example << std::endl << std::endl;
    }

    if (!subcommands_.empty()) {
        std::cout << "Use '" << name_
                  << " <command> --help' for more information about a command."
                  << std::endl;
    }
}

void Command::print_usage() const {
    if (!usage_.empty()) {
        std::cout << "Usage: " << usage_ << std::endl;
    } else {
        std::cout << "Usage: " << name_;
        if (!flags_.empty()) {
            std::cout << " [OPTIONS]";
        }
        if (!subcommands_.empty()) {
            std::cout << " <COMMAND>";
        }
        std::cout << std::endl;
    }
}

// CommandContext implementation
std::string CommandContext::get_flag(const std::string& name) const {
    auto it = flags_.find(name);
    return it != flags_.end() ? it->second : "";
}

bool CommandContext::get_bool_flag(const std::string& name) const {
    std::string value = get_flag(name);
    return value == "true" || value == "1";
}

int CommandContext::get_int_flag(const std::string& name) const {
    std::string value = get_flag(name);
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        return 0;
    }
}

}  // namespace shield::core