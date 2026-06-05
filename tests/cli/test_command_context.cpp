#define BOOST_TEST_MODULE CommandContextTest
#include <boost/test/unit_test.hpp>

#include "shield/cli/command.hpp"

using namespace shield::cli;

BOOST_AUTO_TEST_SUITE(CommandContextTests)

BOOST_AUTO_TEST_CASE(TestSetAndGetFlag) {
    CommandContext ctx;
    ctx.set_flag("config", "app.yaml");
    BOOST_CHECK_EQUAL(ctx.get_flag("config"), "app.yaml");
}

BOOST_AUTO_TEST_CASE(TestGetNonexistentFlag) {
    CommandContext ctx;
    BOOST_CHECK_EQUAL(ctx.get_flag("nonexistent"), "");
}

BOOST_AUTO_TEST_CASE(TestHasFlag) {
    CommandContext ctx;
    BOOST_CHECK(!ctx.has_flag("config"));
    ctx.set_flag("config", "app.yaml");
    BOOST_CHECK(ctx.has_flag("config"));
}

BOOST_AUTO_TEST_CASE(TestBoolFlag) {
    CommandContext ctx;
    ctx.set_flag("verbose", "true");
    BOOST_CHECK(ctx.get_bool_flag("verbose"));

    ctx.set_flag("quiet", "false");
    BOOST_CHECK(!ctx.get_bool_flag("quiet"));

    ctx.set_flag("enabled", "1");
    BOOST_CHECK(ctx.get_bool_flag("enabled"));

    ctx.set_flag("disabled", "0");
    BOOST_CHECK(!ctx.get_bool_flag("disabled"));

    ctx.set_flag("other", "yes");
    BOOST_CHECK(!ctx.get_bool_flag("other"));
}

BOOST_AUTO_TEST_CASE(TestIntFlag) {
    CommandContext ctx;
    ctx.set_flag("port", "8080");
    BOOST_CHECK_EQUAL(ctx.get_int_flag("port"), 8080);
}

BOOST_AUTO_TEST_CASE(TestIntFlagInvalid) {
    CommandContext ctx;
    ctx.set_flag("port", "not_a_number");
    BOOST_CHECK_EQUAL(ctx.get_int_flag("port"), 0);
}

BOOST_AUTO_TEST_CASE(TestIntFlagEmpty) {
    CommandContext ctx;
    BOOST_CHECK_EQUAL(ctx.get_int_flag("port"), 0);
}

BOOST_AUTO_TEST_CASE(TestUserProvidedFlag) {
    CommandContext ctx;
    BOOST_CHECK(!ctx.is_user_provided("config"));
    ctx.set_user_flag("config", "app.yaml");
    BOOST_CHECK(ctx.is_user_provided("config"));
    BOOST_CHECK_EQUAL(ctx.get_flag("config"), "app.yaml");
}

BOOST_AUTO_TEST_CASE(TestPositionalArgs) {
    CommandContext ctx;
    ctx.add_arg("arg1");
    ctx.add_arg("arg2");
    BOOST_CHECK_EQUAL(ctx.args().size(), 2u);
    BOOST_CHECK_EQUAL(ctx.arg(0), "arg1");
    BOOST_CHECK_EQUAL(ctx.arg(1), "arg2");
    BOOST_CHECK_EQUAL(ctx.arg(2), "");  // Out of range
}

BOOST_AUTO_TEST_CASE(TestConfigFile) {
    CommandContext ctx;
    ctx.set_config_file("config/app.yaml");
    BOOST_CHECK_EQUAL(ctx.config_file(), "config/app.yaml");
}

BOOST_AUTO_TEST_SUITE_END()
