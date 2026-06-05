#define BOOST_TEST_MODULE CommandBaseTest
#include <boost/test/unit_test.hpp>

#include "shield/cli/command.hpp"

using namespace shield::cli;

namespace {
class TestCommand : public Command {
public:
    TestCommand(const std::string& name, const std::string& desc)
        : Command(name, desc) {}
    int run(CommandContext& ctx) override {
        last_ctx_ = ctx;
        return 0;
    }
    CommandContext last_ctx_;
};
}  // namespace

BOOST_AUTO_TEST_SUITE(CommandBaseTests)

BOOST_AUTO_TEST_CASE(TestConstruction) {
    TestCommand cmd("test", "A test command");
    BOOST_CHECK_EQUAL(cmd.name(), "test");
    BOOST_CHECK_EQUAL(cmd.description(), "A test command");
    BOOST_CHECK(cmd.long_description().empty());
    BOOST_CHECK(cmd.usage().empty());
    BOOST_CHECK(cmd.subcommands().empty());
}

BOOST_AUTO_TEST_CASE(TestBuilderPattern) {
    TestCommand cmd("test", "desc");
    auto& ref = cmd.set_long_description("long").set_usage("usage [opts]");
    BOOST_CHECK_EQUAL(cmd.long_description(), "long");
    BOOST_CHECK_EQUAL(cmd.usage(), "usage [opts]");
    BOOST_CHECK(&ref == &cmd);
}

BOOST_AUTO_TEST_CASE(TestSetExample) {
    TestCommand cmd("test", "desc");
    cmd.set_example("example text");
    // Example is stored but no getter exposed; just verify no crash
}

BOOST_AUTO_TEST_CASE(TestAddCommand) {
    auto parent = std::make_shared<TestCommand>("parent", "parent cmd");
    auto child = std::make_shared<TestCommand>("child", "child cmd");
    parent->add_command(child);

    BOOST_CHECK_EQUAL(parent->subcommands().size(), 1u);
    BOOST_CHECK_EQUAL(parent->subcommands()[0]->name(), "child");
}

BOOST_AUTO_TEST_CASE(TestFindCommand) {
    auto parent = std::make_shared<TestCommand>("parent", "parent cmd");
    auto child1 = std::make_shared<TestCommand>("alpha", "alpha cmd");
    auto child2 = std::make_shared<TestCommand>("beta", "beta cmd");
    parent->add_command(child1);
    parent->add_command(child2);

    auto found = parent->find_command("beta");
    BOOST_CHECK(found != nullptr);
    BOOST_CHECK_EQUAL(found->name(), "beta");
}

BOOST_AUTO_TEST_CASE(TestFindCommandNotFound) {
    auto parent = std::make_shared<TestCommand>("parent", "parent cmd");
    auto found = parent->find_command("nonexistent");
    BOOST_CHECK(found == nullptr);
}

BOOST_AUTO_TEST_CASE(TestAddFlag) {
    TestCommand cmd("test", "desc");
    cmd.add_flag("output", "Output file", "out.txt");
    // Verify via execute with --help
    char arg0[] = "test";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    // print_help should not crash
    cmd.execute(2, argv);
}

BOOST_AUTO_TEST_CASE(TestAddBoolFlag) {
    TestCommand cmd("test", "desc");
    cmd.add_bool_flag("verbose", "Enable verbose output", false);
    char arg0[] = "test";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    cmd.execute(2, argv);
}

BOOST_AUTO_TEST_CASE(TestAddIntFlag) {
    TestCommand cmd("test", "desc");
    cmd.add_int_flag("port", "Port number", 8080);
    char arg0[] = "test";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    cmd.execute(2, argv);
}

BOOST_AUTO_TEST_CASE(TestAddFlagWithShort) {
    TestCommand cmd("test", "desc");
    cmd.add_flag_with_short("output", "o", "Output file", "out.txt");
    char arg0[] = "test";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    cmd.execute(2, argv);
}

BOOST_AUTO_TEST_CASE(TestAddBoolFlagWithShort) {
    TestCommand cmd("test", "desc");
    cmd.add_bool_flag_with_short("verbose", "v", "Verbose mode", false);
    char arg0[] = "test";
    char arg1[] = "-v";
    char* argv[] = {arg0, arg1};
    int result = cmd.execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_CASE(TestAddIntFlagWithShort) {
    TestCommand cmd("test", "desc");
    cmd.add_int_flag_with_short("port", "p", "Port", 3000);
    char arg0[] = "test";
    char arg1[] = "-p";
    char arg2[] = "9090";
    char* argv[] = {arg0, arg1, arg2};
    int result = cmd.execute(3, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_CASE(TestExecuteHelp) {
    TestCommand cmd("test", "A test command");
    char arg0[] = "test";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    int result = cmd.execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_CASE(TestExecuteShortHelp) {
    TestCommand cmd("test", "A test command");
    char arg0[] = "test";
    char arg1[] = "-h";
    char* argv[] = {arg0, arg1};
    int result = cmd.execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_CASE(TestExecuteNoArgs) {
    TestCommand cmd("test", "A test command");
    char arg0[] = "test";
    char* argv[] = {arg0};
    int result = cmd.execute(1, argv);
    BOOST_CHECK_EQUAL(result, 0);
}

BOOST_AUTO_TEST_CASE(TestSubcommandDelegation) {
    auto parent = std::make_shared<TestCommand>("parent", "parent cmd");
    auto child = std::make_shared<TestCommand>("child", "child cmd");
    parent->add_command(child);

    char arg0[] = "parent";
    char arg1[] = "child";
    char* argv[] = {arg0, arg1};
    int result = parent->execute(2, argv);
    BOOST_CHECK_EQUAL(result, 0);
    // The child should have been executed
    BOOST_CHECK_EQUAL(child->last_ctx_.args().size(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CommandContextTests)

BOOST_AUTO_TEST_CASE(TestSetAndGetFlag) {
    CommandContext ctx;
    ctx.set_flag("key", "value");
    BOOST_CHECK_EQUAL(ctx.get_flag("key"), "value");
    BOOST_CHECK_EQUAL(ctx.get_flag("missing"), "");
}

BOOST_AUTO_TEST_CASE(TestSetUserFlag) {
    CommandContext ctx;
    ctx.set_user_flag("key", "val");
    BOOST_CHECK(ctx.has_flag("key"));
    BOOST_CHECK(ctx.is_user_provided("key"));
    BOOST_CHECK(!ctx.is_user_provided("other"));
}

BOOST_AUTO_TEST_CASE(TestGetBoolFlag) {
    CommandContext ctx;
    ctx.set_flag("a", "true");
    ctx.set_flag("b", "false");
    ctx.set_flag("c", "1");
    ctx.set_flag("d", "0");
    BOOST_CHECK(ctx.get_bool_flag("a"));
    BOOST_CHECK(!ctx.get_bool_flag("b"));
    BOOST_CHECK(ctx.get_bool_flag("c"));
    BOOST_CHECK(!ctx.get_bool_flag("d"));
    BOOST_CHECK(!ctx.get_bool_flag("missing"));
}

BOOST_AUTO_TEST_CASE(TestGetBoolFlagCaseSensitive) {
    CommandContext ctx;
    ctx.set_flag("x", "True");
    BOOST_CHECK(!ctx.get_bool_flag("x"));
}

BOOST_AUTO_TEST_CASE(TestGetIntFlag) {
    CommandContext ctx;
    ctx.set_flag("port", "8080");
    BOOST_CHECK_EQUAL(ctx.get_int_flag("port"), 8080);
    BOOST_CHECK_EQUAL(ctx.get_int_flag("missing"), 0);
    ctx.set_flag("bad", "not_a_number");
    BOOST_CHECK_EQUAL(ctx.get_int_flag("bad"), 0);
}

BOOST_AUTO_TEST_CASE(TestHasFlag) {
    CommandContext ctx;
    BOOST_CHECK(!ctx.has_flag("key"));
    ctx.set_flag("key", "val");
    BOOST_CHECK(ctx.has_flag("key"));
}

BOOST_AUTO_TEST_CASE(TestPositionalArgs) {
    CommandContext ctx;
    BOOST_CHECK(ctx.args().empty());
    BOOST_CHECK_EQUAL(ctx.arg(0), "");

    ctx.add_arg("first");
    ctx.add_arg("second");
    BOOST_CHECK_EQUAL(ctx.args().size(), 2u);
    BOOST_CHECK_EQUAL(ctx.arg(0), "first");
    BOOST_CHECK_EQUAL(ctx.arg(1), "second");
    BOOST_CHECK_EQUAL(ctx.arg(2), "");
}

BOOST_AUTO_TEST_CASE(TestConfigFile) {
    CommandContext ctx;
    BOOST_CHECK(ctx.config_file().empty());
    ctx.set_config_file("app.yaml");
    BOOST_CHECK_EQUAL(ctx.config_file(), "app.yaml");
}

BOOST_AUTO_TEST_SUITE_END()
