#define BOOST_TEST_MODULE ServerCommandTest
#include <boost/test/unit_test.hpp>

#include "shield/commands/server_command.hpp"

using namespace shield::commands;

BOOST_AUTO_TEST_SUITE(ServerCommandTests)

BOOST_AUTO_TEST_CASE(TestConstruction) {
    ServerCommand cmd;
    BOOST_CHECK_EQUAL(cmd.name(), "server");
    BOOST_CHECK(!cmd.description().empty());
}

BOOST_AUTO_TEST_CASE(TestHelp) {
    auto cmd = std::make_shared<ServerCommand>();
    char arg0[] = "server";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    int result = cmd->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_CASE(TestSubcommandsEmpty) {
    ServerCommand cmd;
    BOOST_CHECK(cmd.subcommands().empty());
}

BOOST_AUTO_TEST_SUITE_END()
