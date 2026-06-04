#define BOOST_TEST_MODULE LogConfigTest
#include <boost/test/unit_test.hpp>
#include <stdexcept>

#include "shield/log/log_config.hpp"
#include "shield/log/logger.hpp"

using namespace shield::log;

BOOST_AUTO_TEST_SUITE(LogConfigLevelTests)

BOOST_AUTO_TEST_CASE(TestLevelFromString) {
    BOOST_CHECK(LogConfig::level_from_string("trace") == LogConfig::LogLevel::TRACE);
    BOOST_CHECK(LogConfig::level_from_string("debug") == LogConfig::LogLevel::DEBUG);
    BOOST_CHECK(LogConfig::level_from_string("info") == LogConfig::LogLevel::INFO);
    BOOST_CHECK(LogConfig::level_from_string("warn") == LogConfig::LogLevel::WARN);
    BOOST_CHECK(LogConfig::level_from_string("warning") == LogConfig::LogLevel::WARN);
    BOOST_CHECK(LogConfig::level_from_string("error") == LogConfig::LogLevel::ERROR);
    BOOST_CHECK(LogConfig::level_from_string("fatal") == LogConfig::LogLevel::FATAL);
    BOOST_CHECK(LogConfig::level_from_string("critical") == LogConfig::LogLevel::FATAL);
}

BOOST_AUTO_TEST_CASE(TestLevelFromStringCaseInsensitive) {
    BOOST_CHECK(LogConfig::level_from_string("INFO") == LogConfig::LogLevel::INFO);
    BOOST_CHECK(LogConfig::level_from_string("Debug") == LogConfig::LogLevel::DEBUG);
    BOOST_CHECK(LogConfig::level_from_string("TRACE") == LogConfig::LogLevel::TRACE);
}

BOOST_AUTO_TEST_CASE(TestLevelFromNumeric) {
    BOOST_CHECK(LogConfig::level_from_string("0") == LogConfig::LogLevel::TRACE);
    BOOST_CHECK(LogConfig::level_from_string("1") == LogConfig::LogLevel::DEBUG);
    BOOST_CHECK(LogConfig::level_from_string("2") == LogConfig::LogLevel::INFO);
    BOOST_CHECK(LogConfig::level_from_string("3") == LogConfig::LogLevel::WARN);
    BOOST_CHECK(LogConfig::level_from_string("4") == LogConfig::LogLevel::ERROR);
    BOOST_CHECK(LogConfig::level_from_string("5") == LogConfig::LogLevel::FATAL);
}

BOOST_AUTO_TEST_CASE(TestLevelFromStringInvalidThrows) {
    BOOST_CHECK_THROW(LogConfig::level_from_string("invalid"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(LogConfig::level_from_string(""),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestLevelToString) {
    BOOST_CHECK_EQUAL(LogConfig::level_to_string(LogConfig::LogLevel::TRACE), "trace");
    BOOST_CHECK_EQUAL(LogConfig::level_to_string(LogConfig::LogLevel::DEBUG), "debug");
    BOOST_CHECK_EQUAL(LogConfig::level_to_string(LogConfig::LogLevel::INFO), "info");
    BOOST_CHECK_EQUAL(LogConfig::level_to_string(LogConfig::LogLevel::WARN), "warn");
    BOOST_CHECK_EQUAL(LogConfig::level_to_string(LogConfig::LogLevel::ERROR), "error");
    BOOST_CHECK_EQUAL(LogConfig::level_to_string(LogConfig::LogLevel::FATAL), "fatal");
}

BOOST_AUTO_TEST_CASE(TestLevelRoundTrip) {
    std::vector<std::string> levels = {"trace", "debug", "info", "warn", "error", "fatal"};
    for (const auto& level_str : levels) {
        auto level = LogConfig::level_from_string(level_str);
        BOOST_CHECK_EQUAL(LogConfig::level_to_string(level), level_str);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LogConfigPropertiesTests)

BOOST_AUTO_TEST_CASE(TestPropertiesName) {
    LogConfig config;
    BOOST_CHECK_EQUAL(config.properties_name(), "log");
}

BOOST_AUTO_TEST_CASE(TestDefaultValues) {
    LogConfig config;
    BOOST_CHECK(config.global_level == LogConfig::LogLevel::INFO);
    BOOST_CHECK(config.console.enabled);
    BOOST_CHECK(config.console.colored);
    BOOST_CHECK(config.file.enabled);
    BOOST_CHECK(!config.file.log_file.empty());
    BOOST_CHECK_GT(config.file.max_file_size, 0);
    BOOST_CHECK_GT(config.file.max_files, 0);
}

BOOST_AUTO_TEST_CASE(TestSupportsHotReload) {
    LogConfig config;
    BOOST_CHECK(config.supports_hot_reload());
}

BOOST_AUTO_TEST_CASE(TestShouldLogRespectsGlobalLevel) {
    LogConfig config;
    config.global_level = LogConfig::LogLevel::WARN;

    BOOST_CHECK(!config.should_log(LogConfig::LogLevel::INFO, "test"));
    BOOST_CHECK(!config.should_log(LogConfig::LogLevel::DEBUG, "test"));
    BOOST_CHECK(config.should_log(LogConfig::LogLevel::WARN, "test"));
    BOOST_CHECK(config.should_log(LogConfig::LogLevel::ERROR, "test"));
    BOOST_CHECK(config.should_log(LogConfig::LogLevel::FATAL, "test"));
}

BOOST_AUTO_TEST_CASE(TestShouldLogIncludePatterns) {
    LogConfig config;
    config.global_level = LogConfig::LogLevel::DEBUG;
    config.filter.include_patterns = {"auth", "gateway"};

    BOOST_CHECK(config.should_log(LogConfig::LogLevel::INFO, "auth_service"));
    BOOST_CHECK(config.should_log(LogConfig::LogLevel::INFO, "gateway_handler"));
    BOOST_CHECK(!config.should_log(LogConfig::LogLevel::INFO, "database"));
}

BOOST_AUTO_TEST_CASE(TestShouldLogExcludePatterns) {
    LogConfig config;
    config.global_level = LogConfig::LogLevel::DEBUG;
    config.filter.exclude_patterns = {"noisy_module"};

    BOOST_CHECK(config.should_log(LogConfig::LogLevel::INFO, "normal_module"));
    BOOST_CHECK(!config.should_log(LogConfig::LogLevel::INFO, "noisy_module"));
}

BOOST_AUTO_TEST_CASE(TestValidateValid) {
    LogConfig config;
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestValidateEmptyLogFileThrows) {
    LogConfig config;
    config.file.enabled = true;
    config.file.log_file = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroMaxFileSizeThrows) {
    LogConfig config;
    config.file.enabled = true;
    config.file.max_file_size = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateZeroMaxFilesThrows) {
    LogConfig config;
    config.file.enabled = true;
    config.file.max_files = 0;
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateNetworkEmptyHostThrows) {
    LogConfig config;
    config.network.enabled = true;
    config.network.host = "";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateNetworkInvalidProtocolThrows) {
    LogConfig config;
    config.network.enabled = true;
    config.network.host = "localhost";
    config.network.port = 514;
    config.network.protocol = "http";
    BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TestValidateNetworkValidProtocols) {
    LogConfig config;
    config.network.enabled = true;
    config.network.host = "localhost";
    config.network.port = 514;

    config.network.protocol = "udp";
    BOOST_CHECK_NO_THROW(config.validate());

    config.network.protocol = "tcp";
    BOOST_CHECK_NO_THROW(config.validate());
}

BOOST_AUTO_TEST_CASE(TestFromPtree) {
    // Initialize logger to avoid crash when from_ptree calls Logger::set_level
    LogConfig init_config;
    Logger::init(init_config);
    boost::property_tree::ptree pt;
    pt.put("global_level", "debug");
    pt.put("console.enabled", false);
    pt.put("console.colored", false);
    pt.put("file.enabled", true);
    pt.put("file.log_file", "custom.log");
    pt.put("file.max_file_size", 5242880);
    pt.put("file.max_files", 3);

    LogConfig config;
    config.from_ptree(pt);

    BOOST_CHECK(config.global_level == LogConfig::LogLevel::DEBUG);
    BOOST_CHECK(!config.console.enabled);
    BOOST_CHECK(!config.console.colored);
    BOOST_CHECK(config.file.enabled);
    BOOST_CHECK_EQUAL(config.file.log_file, "custom.log");
    BOOST_CHECK_EQUAL(config.file.max_file_size, 5242880);
    BOOST_CHECK_EQUAL(config.file.max_files, 3);
}

BOOST_AUTO_TEST_SUITE_END()
