// tests/cli/test_command_parsing.cpp
#define BOOST_TEST_MODULE CliCommandParsingTests
#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

#include "shield/cli/command.hpp"

namespace {

class TestConfigCommand final : public shield::cli::Command {
public:
    TestConfigCommand()
        : shield::cli::Command("config", "Test config subcommand") {
        add_flag_with_short("file", "f", "Config file path", "");
    }

    bool file_user_provided = false;
    std::string selected_config;
    std::string global_config_value;

    int run(shield::cli::CommandContext& ctx) override {
        file_user_provided = ctx.is_user_provided("file");
        global_config_value = ctx.get_flag("config");
        selected_config =
            file_user_provided ? ctx.get_flag("file") : global_config_value;
        return 0;
    }
};

class TestRootCommand final : public shield::cli::Command {
public:
    explicit TestRootCommand(std::shared_ptr<TestConfigCommand> config_cmd)
        : shield::cli::Command("shield", "Test root command"),
          config_cmd_(std::move(config_cmd)) {
        add_flag_with_short("config", "c", "Global configuration file",
                            "config/app.yaml");
        add_command(config_cmd_);
    }

    int run(shield::cli::CommandContext&) override { return 0; }

    std::shared_ptr<TestConfigCommand> config_cmd_;
};

struct ArgvBuilder {
    std::vector<std::string> storage;
    std::vector<char*> argv;

    explicit ArgvBuilder(std::initializer_list<const char*> args) {
        storage.reserve(args.size());
        argv.reserve(args.size());
        for (const auto* a : args) {
            storage.emplace_back(a);
        }
        for (auto& s : storage) {
            argv.push_back(s.data());
        }
    }

    int argc() const { return static_cast<int>(argv.size()); }
    char** data() { return argv.data(); }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(CommandParsing)

BOOST_AUTO_TEST_CASE(defaulted_string_flags_are_not_user_provided) {
    auto config_cmd = std::make_shared<TestConfigCommand>();
    auto root = std::make_shared<TestRootCommand>(config_cmd);

    ArgvBuilder args({"shield", "config"});
    BOOST_CHECK_EQUAL(root->execute(args.argc(), args.data()), 0);

    BOOST_CHECK_EQUAL(config_cmd->file_user_provided, false);
    BOOST_CHECK_EQUAL(config_cmd->global_config_value, "config/app.yaml");
    BOOST_CHECK_EQUAL(config_cmd->selected_config, "config/app.yaml");
}

BOOST_AUTO_TEST_CASE(explicit_file_flag_overrides_global_config) {
    auto config_cmd = std::make_shared<TestConfigCommand>();
    auto root = std::make_shared<TestRootCommand>(config_cmd);

    ArgvBuilder args({"shield", "config", "--file", "custom.yaml"});
    BOOST_CHECK_EQUAL(root->execute(args.argc(), args.data()), 0);

    BOOST_CHECK_EQUAL(config_cmd->file_user_provided, true);
    BOOST_CHECK_EQUAL(config_cmd->selected_config, "custom.yaml");
}

BOOST_AUTO_TEST_CASE(global_config_flag_is_respected) {
    auto config_cmd = std::make_shared<TestConfigCommand>();
    auto root = std::make_shared<TestRootCommand>(config_cmd);

    ArgvBuilder args({"shield", "config", "--config", "other.yaml"});
    BOOST_CHECK_EQUAL(root->execute(args.argc(), args.data()), 0);

    BOOST_CHECK_EQUAL(config_cmd->file_user_provided, false);
    BOOST_CHECK_EQUAL(config_cmd->global_config_value, "other.yaml");
    BOOST_CHECK_EQUAL(config_cmd->selected_config, "other.yaml");
}

BOOST_AUTO_TEST_SUITE_END()

