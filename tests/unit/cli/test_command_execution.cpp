#define BOOST_TEST_MODULE CommandExecutionTest
#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

#include "shield/cli/command.hpp"

namespace {

class FixedResultCommand : public shield::cli::Command {
public:
    explicit FixedResultCommand(const std::string& name, int result_code)
        : shield::cli::Command(name, "fixed result command"),
          result_code_(result_code) {}

    int run(shield::cli::CommandContext&) override {
        ++run_count;
        return result_code_;
    }

    int run_count = 0;

private:
    int result_code_;
};

int execute_with_args(shield::cli::Command& command,
                      std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return command.execute(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

BOOST_AUTO_TEST_SUITE(CommandExecutionTests)

BOOST_AUTO_TEST_CASE(ExecutePropagatesRunReturnCode) {
    FixedResultCommand command("top", 7);

    const int rc = execute_with_args(command, {"shield"});

    BOOST_CHECK_EQUAL(rc, 7);
    BOOST_CHECK_EQUAL(command.run_count, 1);
}

BOOST_AUTO_TEST_CASE(ExecutePropagatesSubcommandReturnCode) {
    auto root = std::make_shared<FixedResultCommand>("shield", 0);
    auto subcommand = std::make_shared<FixedResultCommand>("migrate", 3);
    root->add_command(subcommand);

    const int rc = execute_with_args(*root, {"shield", "migrate"});

    BOOST_CHECK_EQUAL(rc, 3);
    BOOST_CHECK_EQUAL(root->run_count, 0);
    BOOST_CHECK_EQUAL(subcommand->run_count, 1);
}

BOOST_AUTO_TEST_SUITE_END()
