#define BOOST_TEST_MODULE CommandsTest
#include <boost/test/unit_test.hpp>

#include "shield/commands/cli_command.hpp"
#include "shield/commands/config_command.hpp"
#include "shield/commands/diagnose_command.hpp"
#include "shield/commands/migrate_command.hpp"

using namespace shield::commands;

BOOST_AUTO_TEST_SUITE(ConfigCommandTests)

BOOST_AUTO_TEST_CASE(TestConfigCommandConstruction) {
    ConfigCommand cmd;
    BOOST_CHECK_EQUAL(cmd.name(), "config");
    BOOST_CHECK(!cmd.description().empty());
    BOOST_CHECK(!cmd.long_description().empty());
    BOOST_CHECK(!cmd.usage().empty());
}

BOOST_AUTO_TEST_CASE(TestConfigCommandHelp) {
    auto cmd = std::make_shared<ConfigCommand>();
    char arg0[] = "config";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    int result = cmd->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(MigrateCommandTests)

BOOST_AUTO_TEST_CASE(TestMigrateCommandConstruction) {
    MigrateCommand cmd;
    BOOST_CHECK_EQUAL(cmd.name(), "migrate");
    BOOST_CHECK(!cmd.description().empty());
    BOOST_CHECK(!cmd.long_description().empty());
}

BOOST_AUTO_TEST_CASE(TestMigrateCommandHelp) {
    auto cmd = std::make_shared<MigrateCommand>();
    char arg0[] = "migrate";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    int result = cmd->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CLICommandTests)

BOOST_AUTO_TEST_CASE(TestCLICommandConstruction) {
    CLICommand cmd;
    BOOST_CHECK_EQUAL(cmd.name(), "cli");
    BOOST_CHECK(!cmd.description().empty());
}

BOOST_AUTO_TEST_CASE(TestCLICommandHelp) {
    auto cmd = std::make_shared<CLICommand>();
    char arg0[] = "cli";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    int result = cmd->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(DiagnoseCommandTests)

BOOST_AUTO_TEST_CASE(TestDiagnoseCommandConstruction) {
    DiagnoseCommand cmd;
    BOOST_CHECK_EQUAL(cmd.name(), "diagnose");
    BOOST_CHECK(!cmd.description().empty());
}

BOOST_AUTO_TEST_CASE(TestDiagnoseCommandHelp) {
    auto cmd = std::make_shared<DiagnoseCommand>();
    char arg0[] = "diagnose";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    int result = cmd->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_SUITE_END()
