#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shield::cli {

// Forward declaration
class CommandContext;

// Base command class (similar to cobra.Command)
class Command : public std::enable_shared_from_this<Command> {
public:
    Command(const std::string& name, const std::string& description);
    virtual ~Command() = default;

    // Core command properties
    std::string name() const { return name_; }
    std::string description() const { return description_; }
    std::string long_description() const { return long_description_; }
    std::string usage() const { return usage_; }

    // Command hierarchy
    void add_command(std::shared_ptr<Command> cmd);
    std::shared_ptr<Command> find_command(const std::string& name);
    const std::vector<std::shared_ptr<Command>>& subcommands() const {
        return subcommands_;
    }

    // Flags and arguments
    void add_flag(const std::string& name, const std::string& description,
                  const std::string& default_value = "");
    void add_flag_with_short(const std::string& name,
                             const std::string& short_name,
                             const std::string& description,
                             const std::string& default_value = "");
    void add_bool_flag(const std::string& name, const std::string& description,
                       bool default_value = false);
    void add_bool_flag_with_short(const std::string& name,
                                  const std::string& short_name,
                                  const std::string& description,
                                  bool default_value = false);
    void add_int_flag(const std::string& name, const std::string& description,
                      int default_value = 0);
    void add_int_flag_with_short(const std::string& name,
                                 const std::string& short_name,
                                 const std::string& description,
                                 int default_value = 0);

    // Command execution
    virtual int run(CommandContext& ctx) = 0;
    int execute(int argc, char* argv[]);

    // Help system
    void print_help() const;
    void print_usage() const;

    // Builder pattern methods
    Command& set_long_description(const std::string& desc) {
        long_description_ = desc;
        return *this;
    }
    Command& set_usage(const std::string& usage) {
        usage_ = usage;
        return *this;
    }
    Command& set_example(const std::string& example) {
        example_ = example;
        return *this;
    }

protected:
    struct Flag {
        std::string name;
        std::string short_name;
        std::string description;
        std::string default_value;
        std::string type;  // "string", "bool", "int"
    };

    std::string name_;
    std::string description_;
    std::string long_description_;
    std::string usage_;
    std::string example_;

    std::vector<std::shared_ptr<Command>> subcommands_;
    std::vector<Flag> flags_;
    Command* parent_ = nullptr;

private:
    std::shared_ptr<Command> parse_and_execute(int argc, char* argv[]);
};

// Command context for passing data between commands
class CommandContext {
public:
    // Flag values
    void set_flag(const std::string& name, const std::string& value) {
        flags_[name] = value;
    }
    void set_user_flag(const std::string& name, const std::string& value) {
        flags_[name] = value;
        user_provided_flags_.insert(name);
    }
    std::string get_flag(const std::string& name) const;
    bool get_bool_flag(const std::string& name) const;
    int get_int_flag(const std::string& name) const;
    bool has_flag(const std::string& name) const {
        return flags_.count(name) > 0;
    }
    bool is_user_provided(const std::string& name) const {
        return user_provided_flags_.count(name) > 0;
    }

    // Positional arguments
    void add_arg(const std::string& arg) { args_.emplace_back(arg); }
    const std::vector<std::string>& args() const { return args_; }
    std::string arg(size_t index) const {
        return index < args_.size() ? args_[index] : "";
    }

    // Global state
    void set_config_file(const std::string& file) { config_file_ = file; }
    const std::string& config_file() const { return config_file_; }

private:
    std::unordered_map<std::string, std::string> flags_;
    std::unordered_set<std::string> user_provided_flags_;
    std::vector<std::string> args_;
    std::string config_file_;
};

}  // namespace shield::core