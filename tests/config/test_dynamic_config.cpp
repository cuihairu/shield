#define BOOST_TEST_MODULE DynamicConfigTest
#include <boost/test/unit_test.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "shield/config/config.hpp"

using namespace shield::config;

BOOST_AUTO_TEST_SUITE(ConfigPathsTests)

BOOST_AUTO_TEST_CASE(TestDefaultConfigFile) {
    BOOST_CHECK_EQUAL(std::string(ConfigPaths::DEFAULT_CONFIG_FILE),
                      "config/app.yaml");
}

BOOST_AUTO_TEST_CASE(TestDefaultConfigDir) {
    BOOST_CHECK_EQUAL(std::string(ConfigPaths::DEFAULT_CONFIG_DIR), "config/");
}

BOOST_AUTO_TEST_CASE(TestGetConfigDir) {
    BOOST_CHECK_EQUAL(ConfigPaths::get_config_dir(), "config/");
}

BOOST_AUTO_TEST_CASE(TestGetProfileConfigFile) {
    BOOST_CHECK_EQUAL(ConfigPaths::get_profile_config_file("dev"),
                      "config/app-dev.yaml");
    BOOST_CHECK_EQUAL(ConfigPaths::get_profile_config_file("prod"),
                      "config/app-prod.yaml");
    BOOST_CHECK_EQUAL(ConfigPaths::get_profile_config_file(""),
                      "config/app-.yaml");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ConfigFormatTests)

BOOST_AUTO_TEST_CASE(TestConfigFormatValues) {
    ConfigFormat yaml = ConfigFormat::YAML;
    ConfigFormat json = ConfigFormat::JSON;
    ConfigFormat ini = ConfigFormat::INI;

    BOOST_CHECK(yaml != json);
    BOOST_CHECK(json != ini);
    BOOST_CHECK(yaml != ini);
}

BOOST_AUTO_TEST_SUITE_END()
