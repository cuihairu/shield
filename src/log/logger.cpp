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

namespace {

std::string normalize_formatter_pattern(std::string pattern) {
    // Heuristic: accept Boost.Log named placeholders by default.
    if (pattern.find("%TimeStamp%") != std::string::npos ||
        pattern.find("%Message%") != std::string::npos ||
        pattern.find("%Severity%") != std::string::npos ||
        pattern.find("%ThreadID%") != std::string::npos) {
        return pattern;
    }

    // Heuristic: translate common spdlog-style patterns to Boost.Log
    // placeholders. Typical spdlog pattern: "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l]
    // %v"
    if (pattern.find("%v") != std::string::npos ||
        pattern.find("%l") != std::string::npos ||
        pattern.find("%t") != std::string::npos ||
        pattern.find("%Y") != std::string::npos) {
        // Replace the first bracketed time-format segment if it looks like a
        // strftime-like pattern (starts with "[%" and contains "%Y").
        auto left = pattern.find("[%");
        if (left != std::string::npos) {
            auto right = pattern.find("]", left);
            if (right != std::string::npos &&
                pattern.substr(left, right - left).find("%Y") !=
                    std::string::npos) {
                pattern.replace(left, right - left + 1, "[%TimeStamp%]");
            }
        }

        // Replace the common short placeholders.
        for (size_t pos = 0;
             (pos = pattern.find("%t", pos)) != std::string::npos;) {
            pattern.replace(pos, 2, "%ThreadID%");
            pos += std::string("%ThreadID%").size();
        }
        for (size_t pos = 0;
             (pos = pattern.find("%l", pos)) != std::string::npos;) {
            pattern.replace(pos, 2, "%Severity%");
            pos += std::string("%Severity%").size();
        }
        for (size_t pos = 0;
             (pos = pattern.find("%v", pos)) != std::string::npos;) {
            pattern.replace(pos, 2, "%Message%");
            pos += std::string("%Message%").size();
        }

        return pattern;
    }

    return pattern;
}

}  // namespace

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
            logging::keywords::format = logging::parse_formatter(
                normalize_formatter_pattern(config.file.pattern)));
    }

    // Setup console sink if enabled
    if (config.console.enabled) {
        logging::add_console_log(
            std::cout,
            logging::keywords::format = logging::parse_formatter(
                normalize_formatter_pattern(config.console.pattern)));
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

void Logger::set_level(LogConfig::LogLevel level) {
    logging::core::get()->set_filter(
        logging::expressions::attr<logging::trivial::severity_level>(
            "Severity") >= to_boost_level(level));
    SHIELD_LOG_INFO << "Log level set to: "
                    << LogConfig::level_to_string(level);
}

}  // namespace shield::log
