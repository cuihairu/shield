// tests/log/test_logger.cpp
#define BOOST_TEST_MODULE LoggerTests
#include <boost/test/unit_test.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/trivial.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/core/null_deleter.hpp>
#include <boost/core/null_deleter.hpp>

#include <sstream> // for capturing log output
#include <string>

#include "shield/core/logger.hpp"
#include "shield/core/log_config.hpp"

namespace logging = boost::log;
namespace sinks = boost::log::sinks;
namespace expr = boost::log::expressions;

// Custom log sink backend that writes logs to stringstream
class StringStreamBackend : public sinks::text_ostream_backend
{
public:
    explicit StringStreamBackend(std::ostream& os)
    {
        add_stream(boost::shared_ptr<std::ostream>(&os, boost::null_deleter()));
    }
};

// Global stringstream for capturing logs
std::stringstream g_log_stream;
boost::shared_ptr<sinks::synchronous_sink<StringStreamBackend>> g_test_sink;

// Test fixture: sets up logging system before each test case, cleans up after
struct LogFixture {
    LogFixture()
    {
        // Clear previous logs
        g_log_stream.str("");
        g_log_stream.clear();

        // Remove all existing sinks
        logging::core::get()->remove_all_sinks();

        // Create and add custom test sink
        g_test_sink = boost::make_shared<sinks::synchronous_sink<StringStreamBackend>>(
            boost::make_shared<StringStreamBackend>(g_log_stream));
        
        // Set sink format
        // Note: using simple format here as we only care about message content and level
        g_test_sink->set_formatter(
            expr::stream
                << expr::attr<logging::trivial::severity_level>("Severity")
                << ": " << expr::smessage
        );
        logging::core::get()->add_sink(g_test_sink);

        // Ensure log level is set to minimum to capture all messages
        logging::core::get()->set_filter(logging::expressions::attr<logging::trivial::severity_level>("Severity") >= logging::trivial::trace);

        // Add common attributes if used in your log patterns
        logging::add_common_attributes();
    }

    ~LogFixture()
    {
        // Remove test sink
        logging::core::get()->remove_sink(g_test_sink);
        g_test_sink.reset();
        // Restore default log settings (optional, depends on your application needs)
        // shield::core::Logger::shutdown(); // if Logger::shutdown() restores default settings
    }
};

// Apply fixture to all test cases
BOOST_GLOBAL_FIXTURE(LogFixture);

BOOST_AUTO_TEST_SUITE(LoggerTestSuite)

BOOST_AUTO_TEST_CASE(test_log_info_message)
{
    SHIELD_LOG_INFO << "This is an info message.";
    BOOST_CHECK(g_log_stream.str().find("info: This is an info message.") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_log_debug_message)
{
    SHIELD_LOG_DEBUG << "This is a debug message.";
    BOOST_CHECK(g_log_stream.str().find("debug: This is a debug message.") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_log_level_filtering)
{
    // Clear log stream
    g_log_stream.str("");
    g_log_stream.clear();

    // Set log level to warning
    logging::core::get()->set_filter(logging::expressions::attr<logging::trivial::severity_level>("Severity") >= logging::trivial::warning);

    SHIELD_LOG_INFO << "This info message should not appear.";
    SHIELD_LOG_WARN << "This warning message should appear.";
    SHIELD_LOG_ERROR << "This error message should also appear.";

    BOOST_CHECK(g_log_stream.str().find("info: This info message should not appear.") == std::string::npos);
    BOOST_CHECK(g_log_stream.str().find("warning: This warning message should appear.") != std::string::npos);
    BOOST_CHECK(g_log_stream.str().find("error: This error message should also appear.") != std::string::npos);

    // Restore log level to trace
    logging::core::get()->set_filter(logging::expressions::attr<logging::trivial::severity_level>("Severity") >= logging::trivial::trace);
}

BOOST_AUTO_TEST_CASE(test_logger_init_shutdown)
{
    // LogFixture has already handled init/shutdown setup and cleanup
    // We can test Logger::init internal log output
    g_log_stream.str(""); // Clear log stream
    g_log_stream.clear();

    // Re-initialize Logger, observe its internal logs
    shield::core::LogConfig config;
    config.level = 2; // info
    config.console_output = true; // ensure console output is considered
    config.log_file = "test_log.log"; // ensure file output is considered

    shield::core::Logger::init(config);
    BOOST_CHECK(g_log_stream.str().find("info: Logger initialized successfully") != std::string::npos);

    g_log_stream.str(""); // Clear log stream
    g_log_stream.clear();
    shield::core::Logger::shutdown();
    BOOST_CHECK(g_log_stream.str().find("info: Logger shutting down") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
