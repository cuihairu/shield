#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shield/config/module_config.hpp"

namespace shield::log {

// 日志配置 - 模块化版本
class LogConfig : public config::ModuleConfig {
public:
    // 日志级别枚举
    enum class LogLevel {
        TRACE = 0,
        DEBUG = 1,
        INFO = 2,
        WARN = 3,
        ERROR = 4,
        FATAL = 5
    };

    // 控制台输出配置
    struct ConsoleConfig {
        bool enabled = true;
        bool colored = true;
        std::string pattern = "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
        LogLevel min_level = LogLevel::INFO;
    };

    // 文件输出配置
    struct FileConfig {
        bool enabled = true;
        std::string log_file = "logs/shield.log";
        int64_t max_file_size = 10485760;  // 10MB
        int max_files = 5;
        bool rotate_on_open = false;
        std::string pattern = "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
        LogLevel min_level = LogLevel::DEBUG;
    };

    // 网络日志配置（如syslog等）
    struct NetworkConfig {
        bool enabled = false;
        std::string protocol = "udp";  // udp, tcp
        std::string host = "localhost";
        uint16_t port = 514;
        std::string facility = "local0";
        LogLevel min_level = LogLevel::WARN;
    };

    // 异步日志配置
    struct AsyncConfig {
        bool enabled = true;
        size_t queue_size = 8192;
        int flush_interval = 1000;           // milliseconds
        bool overflow_policy_block = false;  // false表示丢弃，true表示阻塞
        int worker_threads = 1;
    };

    // 过滤器配置
    struct FilterConfig {
        std::vector<std::string> include_patterns;     // 包含这些模式的日志
        std::vector<std::string> exclude_patterns;     // 排除这些模式的日志
        std::vector<std::string> rate_limit_patterns;  // 限制频率的模式
        int rate_limit_interval = 1000;                // milliseconds
        int rate_limit_burst = 10;                     // 突发数量
    };

    // 配置数据
    LogLevel global_level = LogLevel::INFO;
    ConsoleConfig console;
    FileConfig file;
    NetworkConfig network;
    AsyncConfig async;
    FilterConfig filter;

    // ModuleConfig接口实现
    void from_yaml(const YAML::Node& node) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string module_name() const override { return "log"; }

    // 便利方法
    static LogLevel level_from_string(const std::string& level_str);
    static std::string level_to_string(LogLevel level);
    bool should_log(LogLevel level, const std::string& logger_name = "") const;
};

// 保留旧的简单LogConfig用于向后兼容
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