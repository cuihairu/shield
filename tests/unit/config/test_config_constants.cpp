#define BOOST_TEST_MODULE ConfigConstantsTest
#include <boost/test/unit_test.hpp>

#include "shield/core/config.hpp"

BOOST_AUTO_TEST_SUITE(ConfigConstantsTests)

BOOST_AUTO_TEST_CASE(TestConfigPathConstants) {
    // Test that config path constants are properly defined
    BOOST_CHECK_EQUAL(std::string(shield::core::ConfigPaths::DEFAULT_CONFIG_FILE), 
                      "config/shield.yaml");
    BOOST_CHECK_EQUAL(std::string(shield::core::ConfigPaths::TEST_CONFIG_FILE), 
                      "config/test.yaml");
    BOOST_CHECK_EQUAL(std::string(shield::core::ConfigPaths::DEBUG_CONFIG_FILE), 
                      "config/debug.yaml");
    BOOST_CHECK_EQUAL(std::string(shield::core::ConfigPaths::PRODUCTION_CONFIG_FILE), 
                      "config/production.yaml");
}

BOOST_AUTO_TEST_CASE(TestConfigStaticMethods) {
    // Test static helper methods
    BOOST_CHECK_EQUAL(std::string(shield::core::Config::get_default_config_path()), 
                      "config/shield.yaml");
    BOOST_CHECK_EQUAL(std::string(shield::core::Config::get_test_config_path()), 
                      "config/test.yaml");
}

BOOST_AUTO_TEST_CASE(TestConfigPathsConsistency) {
    // Ensure static methods return the same values as constants
    BOOST_CHECK_EQUAL(shield::core::Config::get_default_config_path(),
                      shield::core::ConfigPaths::DEFAULT_CONFIG_FILE);
    BOOST_CHECK_EQUAL(shield::core::Config::get_test_config_path(),
                      shield::core::ConfigPaths::TEST_CONFIG_FILE);
}

BOOST_AUTO_TEST_SUITE_END()