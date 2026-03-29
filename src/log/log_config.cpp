#include "shield/log/log_config.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "shield/log/logger.hpp"

namespace shield::log {

void LogConfig::from_ptree(const boost::property_tree::ptree& pt) {
    // Backward compatibility:
    // - Accept legacy "level"/"console_output"/"file_output"/"file_path" style
    //   (typically from a "logger" section).
    // - Accept shorthand "level"/"file" under the "log" section.

    // Shorthand/legacy: level -> global_level
    if (!pt.get_optional<std::string>("global_level")) {
        if (auto legacy_level = get_optional_value<std::string>(pt, "level")) {
            global_level = level_from_string(*legacy_level);
            shield::log::Logger::set_level(global_level);
        }
    }

    // Shorthand/legacy: pattern applied to console/file if present
    const auto legacy_pattern = get_optional_value<std::string>(pt, "pattern");

    // Shorthand/legacy console enablement
    if (!pt.get_child_optional("console")) {
        if (auto console_output = get_optional_value<bool>(pt, "console_output"))
            console.enabled = *console_output;
        if (legacy_pattern && !pt.get_optional<std::string>("console.pattern"))
            console.pattern = *legacy_pattern;
    }

    // Shorthand/legacy file settings
    if (!pt.get_child_optional("file")) {
        if (auto file_output = get_optional_value<bool>(pt, "file_output")) {
            file.enabled = *file_output;
        }

        if (auto file_path = get_optional_value<std::string>(pt, "file_path")) {
            file.log_file = *file_path;
            file.enabled = true;
        } else if (auto log_file =
                       get_optional_value<std::string>(pt, "log_file")) {
            file.log_file = *log_file;
            file.enabled = true;
        } else if (auto shorthand_file =
                       get_optional_value<std::string>(pt, "file")) {
            file.log_file = *shorthand_file;
            file.enabled = true;
        }

        if (auto max_file_size =
                get_optional_value<int64_t>(pt, "max_file_size")) {
            file.max_file_size = *max_file_size;
        }
        if (auto max_files = get_optional_value<int>(pt, "max_files")) {
            file.max_files = *max_files;
        }
        if (legacy_pattern && !pt.get_optional<std::string>("file.pattern")) {
            file.pattern = *legacy_pattern;
        }
    }

    // Global level
    if (auto level_str = get_optional_value<std::string>(pt, "global_level")) {
        global_level = level_from_string(*level_str);
        // Dynamically update logger level
        shield::log::Logger::set_level(global_level);
    }

    // Console configuration
    if (auto console_pt = pt.get_child_optional("console")) {
        console.enabled = get_value(*console_pt, "enabled", console.enabled);
        console.colored = get_value(*console_pt, "colored", console.colored);
        console.pattern = get_value(*console_pt, "pattern", console.pattern);
        if (auto min_level_str =
                get_optional_value<std::string>(*console_pt, "min_level"))
            console.min_level = level_from_string(*min_level_str);
    }

    // File configuration
    if (auto file_pt = pt.get_child_optional("file")) {
        // Support shorthand: file: "logs/shield.log"
        if (file_pt->empty() && !file_pt->data().empty()) {
            file.log_file = file_pt->data();
            file.enabled = true;
        }
        file.enabled = get_value(*file_pt, "enabled", file.enabled);
        file.log_file = get_value(*file_pt, "log_file", file.log_file);
        file.max_file_size =
            get_value(*file_pt, "max_file_size", file.max_file_size);
        file.max_files = get_value(*file_pt, "max_files", file.max_files);
        file.rotate_on_open =
            get_value(*file_pt, "rotate_on_open", file.rotate_on_open);
        file.pattern = get_value(*file_pt, "pattern", file.pattern);
        if (auto min_level_str =
                get_optional_value<std::string>(*file_pt, "min_level"))
            file.min_level = level_from_string(*min_level_str);
    }

    // Network configuration
    if (auto network_pt = pt.get_child_optional("network")) {
        network.enabled = get_value(*network_pt, "enabled", network.enabled);
        network.protocol = get_value(*network_pt, "protocol", network.protocol);
        network.host = get_value(*network_pt, "host", network.host);
        network.port = get_value(*network_pt, "port", network.port);
        network.facility = get_value(*network_pt, "facility", network.facility);
        if (auto min_level_str =
                get_optional_value<std::string>(*network_pt, "min_level"))
            network.min_level = level_from_string(*min_level_str);
    }

    // Async configuration
    if (auto async_pt = pt.get_child_optional("async")) {
        async.enabled = get_value(*async_pt, "enabled", async.enabled);
        async.queue_size = get_value(*async_pt, "queue_size", async.queue_size);
        async.flush_interval =
            get_value(*async_pt, "flush_interval", async.flush_interval);
        async.overflow_policy_block = get_value(
            *async_pt, "overflow_policy_block", async.overflow_policy_block);
        async.worker_threads =
            get_value(*async_pt, "worker_threads", async.worker_threads);
    }

    // Filter configuration
    if (auto filter_pt = pt.get_child_optional("filter")) {
        load_vector(*filter_pt, "include_patterns", filter.include_patterns);
        load_vector(*filter_pt, "exclude_patterns", filter.exclude_patterns);
        load_vector(*filter_pt, "rate_limit_patterns",
                    filter.rate_limit_patterns);
        filter.rate_limit_interval = get_value(
            *filter_pt, "rate_limit_interval", filter.rate_limit_interval);
        filter.rate_limit_burst =
            get_value(*filter_pt, "rate_limit_burst", filter.rate_limit_burst);
    }
}

void LogConfig::validate() const {
    if (file.enabled) {
        if (file.log_file.empty()) {
            throw std::invalid_argument(
                "Log file path cannot be empty when file logging is enabled");
        }

        if (file.max_file_size <= 0) {
            throw std::invalid_argument(
                "Log file max_file_size must be greater than 0");
        }

        if (file.max_files <= 0) {
            throw std::invalid_argument(
                "Log file max_files must be greater than 0");
        }
    }

    if (network.enabled) {
        if (network.host.empty()) {
            throw std::invalid_argument(
                "Network log host cannot be empty when network logging is "
                "enabled");
        }

        if (network.port == 0) {
            throw std::invalid_argument(
                "Network log port must be greater than 0");
        }

        if (network.protocol != "udp" && network.protocol != "tcp") {
            throw std::invalid_argument(
                "Network log protocol must be 'udp' or 'tcp'");
        }
    }

    if (async.enabled) {
        if (async.queue_size == 0) {
            throw std::invalid_argument(
                "Async log queue_size must be greater than 0");
        }

        if (async.flush_interval <= 0) {
            throw std::invalid_argument(
                "Async log flush_interval must be greater than 0");
        }

        if (async.worker_threads <= 0) {
            throw std::invalid_argument(
                "Async log worker_threads must be greater than 0");
        }
    }

    if (filter.rate_limit_interval <= 0) {
        throw std::invalid_argument(
            "Filter rate_limit_interval must be greater than 0");
    }

    if (filter.rate_limit_burst <= 0) {
        throw std::invalid_argument(
            "Filter rate_limit_burst must be greater than 0");
    }
}

LogConfig::LogLevel LogConfig::level_from_string(const std::string& level_str) {
    std::string lower_level = level_str;
    std::transform(lower_level.begin(), lower_level.end(), lower_level.begin(),
                   ::tolower);

    // Accept numeric verbosity (0..5) for backward compatibility.
    if (!lower_level.empty() &&
        std::all_of(lower_level.begin(), lower_level.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        int numeric = std::stoi(lower_level);
        switch (numeric) {
            case 0:
                return LogLevel::TRACE;
            case 1:
                return LogLevel::DEBUG;
            case 2:
                return LogLevel::INFO;
            case 3:
                return LogLevel::WARN;
            case 4:
                return LogLevel::ERROR;
            case 5:
                return LogLevel::FATAL;
            default:
                break;
        }
    }

    if (lower_level == "trace") return LogLevel::TRACE;
    if (lower_level == "debug") return LogLevel::DEBUG;
    if (lower_level == "info") return LogLevel::INFO;
    if (lower_level == "warn" || lower_level == "warning")
        return LogLevel::WARN;
    if (lower_level == "error") return LogLevel::ERROR;
    if (lower_level == "fatal" || lower_level == "critical")
        return LogLevel::FATAL;

    throw std::invalid_argument("Invalid log level: " + level_str);
}

std::string LogConfig::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE:
            return "trace";
        case LogLevel::DEBUG:
            return "debug";
        case LogLevel::INFO:
            return "info";
        case LogLevel::WARN:
            return "warn";
        case LogLevel::ERROR:
            return "error";
        case LogLevel::FATAL:
            return "fatal";
        default:
            return "unknown";
    }
}

bool LogConfig::should_log(LogLevel level,
                           const std::string& logger_name) const {
    // Check global level
    if (level < global_level) {
        return false;
    }

    // Check include and exclude patterns
    if (!filter.include_patterns.empty()) {
        bool included = false;
        for (const auto& pattern : filter.include_patterns) {
            if (logger_name.find(pattern) != std::string::npos) {
                included = true;
                break;
            }
        }
        if (!included) return false;
    }

    for (const auto& pattern : filter.exclude_patterns) {
        if (logger_name.find(pattern) != std::string::npos) {
            return false;
        }
    }

    return true;
}

}  // namespace shield::log
