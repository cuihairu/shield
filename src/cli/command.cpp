#include "shield/cli/command.hpp"

#include <algorithm>
#include <boost/program_options.hpp>
#include <iomanip>
#include <iostream>
#include <unordered_map>

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
        return parse_and_execute(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int Command::parse_and_execute(int argc, char* argv[]) {
    CommandContext ctx;

    // Build effective flags list: parent flags first, then this command's flags
    // (child overrides parent on name collisions).
    std::vector<Flag> effective_flags;
    effective_flags.reserve(flags_.size());
    std::unordered_map<std::string, size_t> flag_index_by_name;

    std::vector<const Command*> chain;
    for (auto* cmd = this; cmd != nullptr; cmd = cmd->parent_) {
        chain.push_back(cmd);
    }
    std::reverse(chain.begin(), chain.end());  // root -> leaf

    for (const auto* cmd : chain) {
        for (const auto& flag : cmd->flags_) {
            auto it = flag_index_by_name.find(flag.name);
            if (it == flag_index_by_name.end()) {
                flag_index_by_name.emplace(flag.name, effective_flags.size());
                effective_flags.push_back(flag);
            } else {
                effective_flags[it->second] = flag;
            }
        }
    }

    // Resolve short flag collisions by dropping the earlier short name.
    std::unordered_map<std::string, std::string> short_to_name;
    for (auto& flag : effective_flags) {
        if (flag.short_name.empty()) {
            continue;
        }
        auto it = short_to_name.find(flag.short_name);
        if (it == short_to_name.end()) {
            short_to_name.emplace(flag.short_name, flag.name);
            continue;
        }
        if (it->second != flag.name) {
            // Prefer the later (more specific) flag; drop short name for the
            // earlier one.
            auto earlier = flag_index_by_name.find(it->second);
            if (earlier != flag_index_by_name.end()) {
                effective_flags[earlier->second].short_name.clear();
            }
            it->second = flag.name;
        }
    }

    // Build program options description
    po::options_description desc("Options");
    desc.add_options()("help,h", "Show help message");

    // Add command-specific flags
    for (const auto& flag : effective_flags) {
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
            // For string flags, check if they should have implicit values (like
            // --init)
            if (flag.name == "init") {
                desc.add_options()(option_spec.c_str(),
                                   po::value<std::string>()->implicit_value(
                                       flag.default_value),
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

    // Strip command path tokens from positional args so nested subcommands can
    // be detected when this function is called with the full argv.
    std::vector<std::string> command_path;
    for (auto* cmd = this; cmd != nullptr; cmd = cmd->parent_) {
        command_path.push_back(cmd->name_);
    }
    std::reverse(command_path.begin(), command_path.end());
    if (!command_path.empty()) {
        command_path.erase(command_path.begin());  // root name isn't positional
    }
    for (const auto& name : command_path) {
        if (!unrecognized.empty() && unrecognized.front() == name) {
            unrecognized.erase(unrecognized.begin());
        } else {
            break;
        }
    }

    // Check for subcommands BEFORE processing help/version.
    if (!unrecognized.empty() && !subcommands_.empty()) {
        auto subcmd = find_command(unrecognized.front());
        if (subcmd) {
            // Delegate parsing/execution to the subcommand using full argv so
            // global flags are preserved.
            return subcmd->parse_and_execute(argc, argv);
        }
    }

    // Check for help (only for current command if no subcommand found)
    if (vm.count("help")) {
        print_help();
        return 0;
    }

    // Extract flag values
    for (const auto& flag : effective_flags) {
        if (vm.count(flag.name)) {
            if (flag.type == "bool") {
                ctx.set_user_flag(flag.name, "true");
            } else if (flag.type == "int") {
                const auto value =
                    std::to_string(vm[flag.name].as<int>());
                if (vm[flag.name].defaulted()) {
                    ctx.set_flag(flag.name, value);
                } else {
                    ctx.set_user_flag(flag.name, value);
                }
            } else {
                const auto value = vm[flag.name].as<std::string>();
                if (vm[flag.name].defaulted()) {
                    ctx.set_flag(flag.name, value);
                } else {
                    ctx.set_user_flag(flag.name, value);
                }
            }
        } else {
            ctx.set_flag(flag.name, flag.default_value);
        }
    }

    if (ctx.has_flag("config")) {
        ctx.set_config_file(ctx.get_flag("config"));
    }

    // Add positional arguments to context
    for (const auto& arg : unrecognized) {
        ctx.add_arg(arg);
    }

    // Execute this command
    return run(ctx);
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

    if (parent_ != nullptr && !parent_->flags_.empty()) {
        std::cout << "Global Flags:" << std::endl;
        for (const auto& flag : parent_->flags_) {
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

}  // namespace shield::cli
