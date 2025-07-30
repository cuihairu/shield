#include "shield/log/logger.hpp"

#include <boost/log/attributes/current_thread_id.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <filesystem>
#include <iostream>

namespace logging = boost::log;
namespace sinks = boost::log::sinks;
namespace expr = boost::log::expressions;
namespace attrs = boost::log::attributes;

namespace shield::log {

LogConfig Logger::config_;

// Helper function to map LogLevel enum to boost::log::trivial::severity_level
logging::trivial::severity_level to_boost_level(LogConfig::LogLevel level) {
    switch (level) {
        case LogConfig::LogLevel::TRACE:
            return logging::trivial::trace;
        case LogConfig::LogLevel::DEBUG:
            return logging::trivial::debug;
        case LogConfig::LogLevel::INFO:
            return logging::trivial::info;
        case LogConfig::LogLevel::WARN:
            return logging::trivial::warning;
        case LogConfig::LogLevel::ERROR:
            return logging::trivial::error;
        case LogConfig::LogLevel::FATAL:
            return logging::trivial::fatal;
        default:
            return logging::trivial::info;
    }
}

void Logger::init(const LogConfig& config) {
    config_ = config;

    // Create log directory if file logging is enabled
    if (config.file.enabled) {
        std::filesystem::path log_path(config.file.log_file);
        auto parent_path = log_path.parent_path();
        if (!parent_path.empty()) {
            std::filesystem::create_directories(parent_path);
        }

        // Setup file sink
        auto file_sink = logging::add_file_log(
            logging::keywords::file_name = config.file.log_file,
            logging::keywords::rotation_size = config.file.max_file_size,
            logging::keywords::time_based_rotation =
                sinks::file::rotation_at_time_point(0, 0, 0),  // Rotate daily
            logging::keywords::max_files =
                config.file.max_files,  // Keep max_files files
            logging::keywords::format =
                logging::parse_formatter(config.file.pattern));
    }

    // Setup console sink if enabled
    if (config.console.enabled) {
        logging::add_console_log(
            std::cout, logging::keywords::format =
                           logging::parse_formatter(config.console.pattern));
    }

    // Add common attributes
    logging::add_common_attributes();

    // Set log level based on global level
    logging::core::get()->set_filter(
        logging::expressions::attr<logging::trivial::severity_level>(
            "Severity") >= to_boost_level(config.global_level));

    // Initialize default logger
    BOOST_LOG_TRIVIAL(info) << "Logger initialized successfully";
}

void Logger::shutdown() {
    BOOST_LOG_TRIVIAL(info) << "Logger shutting down";
    logging::core::get()->remove_all_sinks();
}

LogConfig::LogLevel Logger::level_from_string(const std::string& level_str) {
    return LogConfig::level_from_string(level_str);
}

// TODO: Implement get_logger, set_level, get_level for boost::log
// For now, we'll use the default logger directly.

}  // namespace shield::log