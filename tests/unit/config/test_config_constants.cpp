#define BOOST_TEST_MODULE ConfigConstantsTest
#include <boost/test/unit_test.hpp>

#include "shield/config/config.hpp"

using namespace shield::config;

BOOST_AUTO_TEST_SUITE(ConfigConstantsTests)

BOOST_AUTO_TEST_CASE(TestConfigPathConstants) {
    // Test that config path constants are properly defined
    BOOST_CHECK_EQUAL(std::string(ConfigPaths::DEFAULT_CONFIG_FILE),
                      "config/app.yaml");
    BOOST_CHECK_EQUAL(std::string(ConfigPaths::DEFAULT_CONFIG_DIR), "config/");
}

BOOST_AUTO_TEST_CASE(TestGetConfigDir) {
    BOOST_CHECK_EQUAL(ConfigPaths::get_config_dir(), "config/");
}

BOOST_AUTO_TEST_CASE(TestGetProfileConfigFile) {
    BOOST_CHECK_EQUAL(ConfigPaths::get_profile_config_file("dev"),
                      "config/app-dev.yaml");
    BOOST_CHECK_EQUAL(ConfigPaths::get_profile_config_file("production"),
                      "config/app-production.yaml");
    BOOST_CHECK_EQUAL(ConfigPaths::get_profile_config_file("test"),
                      "config/app-test.yaml");
}

BOOST_AUTO_TEST_CASE(TestConfigFormatEnum) {
    // Verify config format enum values exist
    ConfigFormat yaml = ConfigFormat::YAML;
    ConfigFormat json = ConfigFormat::JSON;
    ConfigFormat ini = ConfigFormat::INI;
    BOOST_CHECK(yaml != json);
    BOOST_CHECK(json != ini);
}

BOOST_AUTO_TEST_SUITE_END()
