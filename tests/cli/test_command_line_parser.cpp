#define BOOST_TEST_MODULE CommandLineParserTest
#include <boost/test/unit_test.hpp>

#include "shield/cli/command_line_parser.hpp"

using namespace shield::cli;

BOOST_AUTO_TEST_SUITE(CommandLineParserTests)

BOOST_AUTO_TEST_CASE(TestParseNoArgs) {
    char arg0[] = "shield";
    char* argv[] = {arg0};
    auto options = CommandLineParser::parse(1, argv);
    BOOST_CHECK(options.subcommand == SubCommand::Server);
    BOOST_CHECK(!options.show_version);
    BOOST_CHECK(!options.show_help);
}

BOOST_AUTO_TEST_CASE(TestParseVersionFlag) {
    char arg0[] = "shield";
    char arg1[] = "--version";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.show_version);
}

BOOST_AUTO_TEST_CASE(TestParseHelpFlag) {
    char arg0[] = "shield";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.show_help);
}

BOOST_AUTO_TEST_CASE(TestParseServerSubcommand) {
    char arg0[] = "shield";
    char arg1[] = "server";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.subcommand == SubCommand::Server);
}

BOOST_AUTO_TEST_CASE(TestParseCliSubcommand) {
    char arg0[] = "shield";
    char arg1[] = "cli";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.subcommand == SubCommand::CLI);
}

BOOST_AUTO_TEST_CASE(TestParseMigrateSubcommand) {
    char arg0[] = "shield";
    char arg1[] = "migrate";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.subcommand == SubCommand::Migrate);
}

BOOST_AUTO_TEST_CASE(TestParseTestSubcommand) {
    char arg0[] = "shield";
    char arg1[] = "test";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.subcommand == SubCommand::Test);
}

BOOST_AUTO_TEST_CASE(TestParseConfigSubcommand) {
    char arg0[] = "shield";
    char arg1[] = "config";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.subcommand == SubCommand::Config);
}

BOOST_AUTO_TEST_CASE(TestParseUnknownSubcommand) {
    char arg0[] = "shield";
    char arg1[] = "unknown";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    // Unknown subcommand defaults to Server
    BOOST_CHECK(options.subcommand == SubCommand::Server);
}

BOOST_AUTO_TEST_CASE(TestParseGlobalConfigFlag) {
    char arg0[] = "shield";
    char arg1[] = "--config";
    char arg2[] = "app.yaml";
    char* argv[] = {arg0, arg1, arg2};
    auto options = CommandLineParser::parse(3, argv);
    BOOST_CHECK_EQUAL(options.config_file, "app.yaml");
}

BOOST_AUTO_TEST_CASE(TestParseShortVersionFlag) {
    char arg0[] = "shield";
    char arg1[] = "-v";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.show_version);
}

BOOST_AUTO_TEST_CASE(TestParseShortHelpFlag) {
    char arg0[] = "shield";
    char arg1[] = "-h";
    char* argv[] = {arg0, arg1};
    auto options = CommandLineParser::parse(2, argv);
    BOOST_CHECK(options.show_help);
}

BOOST_AUTO_TEST_CASE(TestParseShortConfigFlag) {
    char arg0[] = "shield";
    char arg1[] = "-c";
    char arg2[] = "prod.yaml";
    char* argv[] = {arg0, arg1, arg2};
    auto options = CommandLineParser::parse(3, argv);
    BOOST_CHECK_EQUAL(options.config_file, "prod.yaml");
}

BOOST_AUTO_TEST_CASE(TestParseServerWithPort) {
    char arg0[] = "shield";
    char arg1[] = "server";
    char arg2[] = "--port";
    char arg3[] = "9090";
    char* argv[] = {arg0, arg1, arg2, arg3};
    auto options = CommandLineParser::parse(4, argv);
    BOOST_CHECK(options.subcommand == SubCommand::Server);
    auto it = options.subcommand_args.find("port");
    BOOST_CHECK(it != options.subcommand_args.end());
    BOOST_CHECK_EQUAL(it->second, "9090");
}

BOOST_AUTO_TEST_CASE(TestCommandLineOptionsDefaults) {
    CommandLineOptions options;
    BOOST_CHECK(!options.show_version);
    BOOST_CHECK(!options.show_help);
    BOOST_CHECK(options.config_file.empty());
    BOOST_CHECK(options.subcommand == SubCommand::None);
    BOOST_CHECK(options.subcommand_args.empty());
    BOOST_CHECK(options.positional_args.empty());
}

BOOST_AUTO_TEST_CASE(TestSubCommandEnumValues) {
    // Verify enum distinctness
    BOOST_CHECK(SubCommand::None != SubCommand::Server);
    BOOST_CHECK(SubCommand::Server != SubCommand::CLI);
    BOOST_CHECK(SubCommand::CLI != SubCommand::Migrate);
    BOOST_CHECK(SubCommand::Migrate != SubCommand::Test);
    BOOST_CHECK(SubCommand::Test != SubCommand::Config);
}

BOOST_AUTO_TEST_SUITE_END()
