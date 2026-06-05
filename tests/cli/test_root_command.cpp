#define BOOST_TEST_MODULE RootCommandTest
#include <boost/test/unit_test.hpp>

#include "shield/cli/root_command.hpp"

using namespace shield::cli;

BOOST_AUTO_TEST_SUITE(RootCommandTests)

BOOST_AUTO_TEST_CASE(TestCreate) {
    auto root = RootCommand::create();
    BOOST_CHECK(root != nullptr);
    BOOST_CHECK_EQUAL(root->name(), "shield");
    BOOST_CHECK(!root->description().empty());
    BOOST_CHECK(!root->long_description().empty());
    BOOST_CHECK(!root->usage().empty());
}

BOOST_AUTO_TEST_CASE(TestHasSubcommands) {
    auto root = RootCommand::create();
    // Should have: server, cli, config, diagnose, migrate
    BOOST_CHECK(root->subcommands().size() >= 5);
}

BOOST_AUTO_TEST_CASE(TestFindServerSubcommand) {
    auto root = RootCommand::create();
    auto server = root->find_command("server");
    BOOST_CHECK(server != nullptr);
    BOOST_CHECK_EQUAL(server->name(), "server");
}

BOOST_AUTO_TEST_CASE(TestFindCliSubcommand) {
    auto root = RootCommand::create();
    auto cli = root->find_command("cli");
    BOOST_CHECK(cli != nullptr);
    BOOST_CHECK_EQUAL(cli->name(), "cli");
}

BOOST_AUTO_TEST_CASE(TestFindConfigSubcommand) {
    auto root = RootCommand::create();
    auto config = root->find_command("config");
    BOOST_CHECK(config != nullptr);
    BOOST_CHECK_EQUAL(config->name(), "config");
}

BOOST_AUTO_TEST_CASE(TestFindDiagnoseSubcommand) {
    auto root = RootCommand::create();
    auto diagnose = root->find_command("diagnose");
    BOOST_CHECK(diagnose != nullptr);
    BOOST_CHECK_EQUAL(diagnose->name(), "diagnose");
}

BOOST_AUTO_TEST_CASE(TestFindMigrateSubcommand) {
    auto root = RootCommand::create();
    auto migrate = root->find_command("migrate");
    BOOST_CHECK(migrate != nullptr);
    BOOST_CHECK_EQUAL(migrate->name(), "migrate");
}

BOOST_AUTO_TEST_CASE(TestFindUnknownSubcommand) {
    auto root = RootCommand::create();
    auto unknown = root->find_command("nonexistent");
    BOOST_CHECK(unknown == nullptr);
}

BOOST_AUTO_TEST_CASE(TestHelpFlag) {
    auto root = RootCommand::create();
    char arg0[] = "shield";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    int result = root->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_CASE(TestVersionFlag) {
    auto root = RootCommand::create();
    char arg0[] = "shield";
    char arg1[] = "--version";
    char* argv[] = {arg0, arg1};
    int result = root->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CommandRegistryTests)

BOOST_AUTO_TEST_CASE(TestCreateRootCommand) {
    auto root = CommandRegistry::create_root_command();
    BOOST_CHECK(root != nullptr);
    BOOST_CHECK_EQUAL(root->name(), "shield");
}

BOOST_AUTO_TEST_SUITE_END()
