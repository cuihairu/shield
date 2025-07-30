#include "shield/log/log_config.hpp"

#include <algorithm>
#include <stdexcept>

#include "shield/log/logger.hpp"

namespace shield::log {

void LogConfig::from_ptree(const boost::property_tree::ptree& pt) {
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

YAML::Node LogConfig::to_yaml() const {
    YAML::Node node;

    // Global level
    node["global_level"] = level_to_string(global_level);

    // Console configuration
    node["console"]["enabled"] = console.enabled;
    node["console"]["colored"] = console.colored;
    node["console"]["pattern"] = console.pattern;
    node["console"]["min_level"] = level_to_string(console.min_level);

    // File configuration
    node["file"]["enabled"] = file.enabled;
    node["file"]["log_file"] = file.log_file;
    node["file"]["max_file_size"] = file.max_file_size;
    node["file"]["max_files"] = file.max_files;
    node["file"]["rotate_on_open"] = file.rotate_on_open;
    node["file"]["pattern"] = file.pattern;
    node["file"]["min_level"] = level_to_string(file.min_level);

    // Network configuration
    node["network"]["enabled"] = network.enabled;
    node["network"]["protocol"] = network.protocol;
    node["network"]["host"] = network.host;
    node["network"]["port"] = network.port;
    node["network"]["facility"] = network.facility;
    node["network"]["min_level"] = level_to_string(network.min_level);

    // Async configuration
    node["async"]["enabled"] = async.enabled;
    node["async"]["queue_size"] = async.queue_size;
    node["async"]["flush_interval"] = async.flush_interval;
    node["async"]["overflow_policy_block"] = async.overflow_policy_block;
    node["async"]["worker_threads"] = async.worker_threads;

    // Filter configuration
    for (const auto& pattern : filter.include_patterns) {
        node["filter"]["include_patterns"].push_back(pattern);
    }
    for (const auto& pattern : filter.exclude_patterns) {
        node["filter"]["exclude_patterns"].push_back(pattern);
    }
    for (const auto& pattern : filter.rate_limit_patterns) {
        node["filter"]["rate_limit_patterns"].push_back(pattern);
    }
    node["filter"]["rate_limit_interval"] = filter.rate_limit_interval;
    node["filter"]["rate_limit_burst"] = filter.rate_limit_burst;

    return node;
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