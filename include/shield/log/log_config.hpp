#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shield/config/config.hpp"

namespace shield::log {

// Log configuration - modularized version
class LogConfig : public config::ReloadableConfigurationProperties<LogConfig> {
public:
    // Log level enumeration
    enum class LogLevel {
        TRACE = 0,
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
        FATAL = 5
    };

    // Console output configuration
    struct ConsoleConfig {
        bool enabled = true;
        bool colored = true;
        std::string pattern =
            "[%TimeStamp%] [%ThreadID%] [%Severity%] %Message%";
        LogLevel min_level = LogLevel::INFO;
    };

    // File output configuration
    struct FileConfig {
        bool enabled = true;
        std::string log_file = "logs/shield.log";
        int64_t max_file_size = 10485760;  // 10MB
        int max_files = 5;
        bool rotate_on_open = false;
        std::string pattern =
            "[%TimeStamp%] [%ThreadID%] [%Severity%] %Message%";
        LogLevel min_level = LogLevel::DEBUG;
    };

    // Network logging configuration (e.g., syslog)
    struct NetworkConfig {
        bool enabled = false;
        std::string protocol = "udp";  // udp, tcp
        std::string host = "localhost";
        uint16_t port = 514;
        std::string facility = "local0";
        LogLevel min_level = LogLevel::WARN;
    };

    // Asynchronous logging configuration
    struct AsyncConfig {
        bool enabled = true;
        size_t queue_size = 8192;
        int flush_interval = 1000;  // milliseconds
        bool overflow_policy_block =
            false;  // false means drop, true means block
        int worker_threads = 1;
    };

    // Filter configuration
    struct FilterConfig {
        std::vector<std::string>
            include_patterns;  // Log messages matching these patterns
        std::vector<std::string>
            exclude_patterns;  // Log messages excluding these patterns
        std::vector<std::string>
            rate_limit_patterns;         // Patterns for rate limiting
        int rate_limit_interval = 1000;  // milliseconds
        int rate_limit_burst = 10;       // Burst count
    };

    // Configuration data
    LogLevel global_level = LogLevel::INFO;
    ConsoleConfig console;
    FileConfig file;
    NetworkConfig network;
    AsyncConfig async;
    FilterConfig filter;

    // ComponentConfig interface implementation
    void from_ptree(const boost::property_tree::ptree& pt) override;
    void validate() const override;
    std::string properties_name() const override { return "log"; }

    // Helper methods for string/enum conversion
    static LogLevel level_from_string(const std::string& level_str);
    static std::string level_to_string(LogLevel level);
    bool should_log(LogLevel level, const std::string& logger_name) const;
};

// Retain old simple LogConfig for backward compatibility
struct LegacyLogConfig {
    // Log level
    int level{0};  // corresponds to trace level

    // Log file path
    std::string log_file{"logs/shield.log"};

    // Maximum log file size (bytes)
    size_t max_file_size{1024 * 1024 * 100};  // 100MB

    // Number of log files to retain
    size_t max_files{5};

    // Whether to output to console simultaneously
    bool console_output{true};

    // Log format
    std::string pattern{"[%TimeStamp%] [%Severity%] %Message%"};
};

}  // namespace shield::log
