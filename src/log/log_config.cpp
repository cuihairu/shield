#include "shield/log/log_config.hpp"

#include <algorithm>
#include <stdexcept>

namespace shield::log {

void LogConfig::from_yaml(const YAML::Node& node) {
    // 全局级别
    if (node["global_level"]) {
        global_level =
            level_from_string(node["global_level"].as<std::string>());
    }

    // 控制台配置
    if (node["console"]) {
        auto console_node = node["console"];
        if (console_node["enabled"])
            console.enabled = console_node["enabled"].as<bool>();
        if (console_node["colored"])
            console.colored = console_node["colored"].as<bool>();
        if (console_node["pattern"])
            console.pattern = console_node["pattern"].as<std::string>();
        if (console_node["min_level"])
            console.min_level =
                level_from_string(console_node["min_level"].as<std::string>());
    }

    // 文件配置
    if (node["file"]) {
        auto file_node = node["file"];
        if (file_node["enabled"])
            file.enabled = file_node["enabled"].as<bool>();
        if (file_node["log_file"])
            file.log_file = file_node["log_file"].as<std::string>();
        if (file_node["max_file_size"])
            file.max_file_size = file_node["max_file_size"].as<int64_t>();
        if (file_node["max_files"])
            file.max_files = file_node["max_files"].as<int>();
        if (file_node["rotate_on_open"])
            file.rotate_on_open = file_node["rotate_on_open"].as<bool>();
        if (file_node["pattern"])
            file.pattern = file_node["pattern"].as<std::string>();
        if (file_node["min_level"])
            file.min_level =
                level_from_string(file_node["min_level"].as<std::string>());
    }

    // 网络配置
    if (node["network"]) {
        auto network_node = node["network"];
        if (network_node["enabled"])
            network.enabled = network_node["enabled"].as<bool>();
        if (network_node["protocol"])
            network.protocol = network_node["protocol"].as<std::string>();
        if (network_node["host"])
            network.host = network_node["host"].as<std::string>();
        if (network_node["port"])
            network.port = network_node["port"].as<uint16_t>();
        if (network_node["facility"])
            network.facility = network_node["facility"].as<std::string>();
        if (network_node["min_level"])
            network.min_level =
                level_from_string(network_node["min_level"].as<std::string>());
    }

    // 异步配置
    if (node["async"]) {
        auto async_node = node["async"];
        if (async_node["enabled"])
            async.enabled = async_node["enabled"].as<bool>();
        if (async_node["queue_size"])
            async.queue_size = async_node["queue_size"].as<size_t>();
        if (async_node["flush_interval"])
            async.flush_interval = async_node["flush_interval"].as<int>();
        if (async_node["overflow_policy_block"])
            async.overflow_policy_block =
                async_node["overflow_policy_block"].as<bool>();
        if (async_node["worker_threads"])
            async.worker_threads = async_node["worker_threads"].as<int>();
    }

    // 过滤器配置
    if (node["filter"]) {
        auto filter_node = node["filter"];
        if (filter_node["include_patterns"]) {
            for (const auto& pattern : filter_node["include_patterns"]) {
                filter.include_patterns.push_back(pattern.as<std::string>());
            }
        }
        if (filter_node["exclude_patterns"]) {
            for (const auto& pattern : filter_node["exclude_patterns"]) {
                filter.exclude_patterns.push_back(pattern.as<std::string>());
            }
        }
        if (filter_node["rate_limit_patterns"]) {
            for (const auto& pattern : filter_node["rate_limit_patterns"]) {
                filter.rate_limit_patterns.push_back(pattern.as<std::string>());
            }
        }
        if (filter_node["rate_limit_interval"])
            filter.rate_limit_interval =
                filter_node["rate_limit_interval"].as<int>();
        if (filter_node["rate_limit_burst"])
            filter.rate_limit_burst = filter_node["rate_limit_burst"].as<int>();
    }
}

YAML::Node LogConfig::to_yaml() const {
    YAML::Node node;

    // 全局级别
    node["global_level"] = level_to_string(global_level);

    // 控制台配置
    node["console"]["enabled"] = console.enabled;
    node["console"]["colored"] = console.colored;
    node["console"]["pattern"] = console.pattern;
    node["console"]["min_level"] = level_to_string(console.min_level);

    // 文件配置
    node["file"]["enabled"] = file.enabled;
    node["file"]["log_file"] = file.log_file;
    node["file"]["max_file_size"] = file.max_file_size;
    node["file"]["max_files"] = file.max_files;
    node["file"]["rotate_on_open"] = file.rotate_on_open;
    node["file"]["pattern"] = file.pattern;
    node["file"]["min_level"] = level_to_string(file.min_level);

    // 网络配置
    node["network"]["enabled"] = network.enabled;
    node["network"]["protocol"] = network.protocol;
    node["network"]["host"] = network.host;
    node["network"]["port"] = network.port;
    node["network"]["facility"] = network.facility;
    node["network"]["min_level"] = level_to_string(network.min_level);

    // 异步配置
    node["async"]["enabled"] = async.enabled;
    node["async"]["queue_size"] = async.queue_size;
    node["async"]["flush_interval"] = async.flush_interval;
    node["async"]["overflow_policy_block"] = async.overflow_policy_block;
    node["async"]["worker_threads"] = async.worker_threads;

    // 过滤器配置
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
    // 检查全局级别
    if (level < global_level) {
        return false;
    }

    // 检查包含和排除模式
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