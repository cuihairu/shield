#include "shield/log/dynamic_log_config.hpp"

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <iostream>

#include "shield/log/logger.hpp"

namespace shield::log {

DynamicLogConfigManager& DynamicLogConfigManager::instance() {
    static DynamicLogConfigManager instance;
    return instance;
}

void DynamicLogConfigManager::initialize() {
    register_dynamic_fields();

    // 注册为日志模块的配置变更监听器
    auto& dynamic_config = config::DynamicConfigManager::instance();
    dynamic_config.add_listener("log",
                                std::shared_ptr<config::ConfigChangeListener>(
                                    this, [](config::ConfigChangeListener*) {
                                    }));  // 使用空删除器，因为这是单例
}

void DynamicLogConfigManager::register_dynamic_fields() {
    auto& dynamic_config = config::DynamicConfigManager::instance();

    // 日志级别验证器
    auto level_validator = [](const std::string& value) -> bool {
        return value == "trace" || value == "debug" || value == "info" ||
               value == "warn" || value == "error" || value == "fatal";
    };

    // 布尔值验证器
    auto bool_validator = [](const std::string& value) -> bool {
        return value == "true" || value == "false" || value == "1" ||
               value == "0";
    };

    // 注册动态配置字段
    dynamic_config.register_field(
        "log", "global_level", config::ConfigChangePolicy::DYNAMIC,
        "Global log level (trace/debug/info/warn/error/fatal)",
        level_validator);

    dynamic_config.register_field(
        "log", "console_level", config::ConfigChangePolicy::DYNAMIC,
        "Console output minimum log level", level_validator);

    dynamic_config.register_field(
        "log", "file_level", config::ConfigChangePolicy::DYNAMIC,
        "File output minimum log level", level_validator);

    dynamic_config.register_field(
        "log", "console_enabled", config::ConfigChangePolicy::DYNAMIC,
        "Enable/disable console logging", bool_validator);

    dynamic_config.register_field(
        "log", "file_enabled", config::ConfigChangePolicy::DYNAMIC,
        "Enable/disable file logging", bool_validator);

    // 静态配置示例（不可运行时修改）
    dynamic_config.register_field("log", "log_file_path",
                                  config::ConfigChangePolicy::STATIC,
                                  "Log file path (requires restart to change)");

    dynamic_config.register_field(
        "log", "max_file_size", config::ConfigChangePolicy::HOT_RELOAD,
        "Maximum log file size (requires component restart)");
}

bool DynamicLogConfigManager::set_global_level(LogConfig::LogLevel level) {
    std::string level_str = level_to_string(level);
    auto& dynamic_config = config::DynamicConfigManager::instance();
    return dynamic_config.set_config("log", "global_level", level_str);
}

bool DynamicLogConfigManager::set_console_level(LogConfig::LogLevel level) {
    std::string level_str = level_to_string(level);
    auto& dynamic_config = config::DynamicConfigManager::instance();
    return dynamic_config.set_config("log", "console_level", level_str);
}

bool DynamicLogConfigManager::set_file_level(LogConfig::LogLevel level) {
    std::string level_str = level_to_string(level);
    auto& dynamic_config = config::DynamicConfigManager::instance();
    return dynamic_config.set_config("log", "file_level", level_str);
}

bool DynamicLogConfigManager::enable_console(bool enabled) {
    auto& dynamic_config = config::DynamicConfigManager::instance();
    return dynamic_config.set_config("log", "console_enabled", enabled);
}

bool DynamicLogConfigManager::enable_file(bool enabled) {
    auto& dynamic_config = config::DynamicConfigManager::instance();
    return dynamic_config.set_config("log", "file_enabled", enabled);
}

LogConfig::LogLevel DynamicLogConfigManager::get_global_level() const {
    return current_global_level_.load();
}

LogConfig::LogLevel DynamicLogConfigManager::get_console_level() const {
    return current_console_level_.load();
}

LogConfig::LogLevel DynamicLogConfigManager::get_file_level() const {
    return current_file_level_.load();
}

bool DynamicLogConfigManager::is_console_enabled() const {
    return console_enabled_.load();
}

bool DynamicLogConfigManager::is_file_enabled() const {
    return file_enabled_.load();
}

void DynamicLogConfigManager::on_config_changed(const std::string& field_name,
                                                const std::string& old_value,
                                                const std::string& new_value) {
    std::cout << "Log config changed: " << field_name << " from '" << old_value
              << "' to '" << new_value << "'" << std::endl;

    if (field_name == "global_level") {
        LogConfig::LogLevel new_level = level_from_string(new_value);
        current_global_level_.store(new_level);
        apply_level_change(new_level);

    } else if (field_name == "console_level") {
        LogConfig::LogLevel new_level = level_from_string(new_value);
        current_console_level_.store(new_level);
        // TODO: 更新控制台sink的过滤器

    } else if (field_name == "file_level") {
        LogConfig::LogLevel new_level = level_from_string(new_value);
        current_file_level_.store(new_level);
        // TODO: 更新文件sink的过滤器

    } else if (field_name == "console_enabled") {
        bool enabled = (new_value == "true" || new_value == "1");
        console_enabled_.store(enabled);
        // TODO: 启用/禁用控制台sink

    } else if (field_name == "file_enabled") {
        bool enabled = (new_value == "true" || new_value == "1");
        file_enabled_.store(enabled);
        // TODO: 启用/禁用文件sink
    }
}

void DynamicLogConfigManager::apply_level_change(
    LogConfig::LogLevel new_level) {
    // 更新boost::log的全局过滤器
    boost::log::core::get()->set_filter(
        boost::log::expressions::attr<boost::log::trivial::severity_level>(
            "Severity") >=
            [new_level]() -> boost::log::trivial::severity_level {
            switch (new_level) {
                case LogConfig::LogLevel::TRACE:
                    return boost::log::trivial::trace;
                case LogConfig::LogLevel::DEBUG:
                    return boost::log::trivial::debug;
                case LogConfig::LogLevel::INFO:
                    return boost::log::trivial::info;
                case LogConfig::LogLevel::WARN:
                    return boost::log::trivial::warning;
                case LogConfig::LogLevel::ERROR:
                    return boost::log::trivial::error;
                case LogConfig::LogLevel::FATAL:
                    return boost::log::trivial::fatal;
                default:
                    return boost::log::trivial::info;
            }
        }());

    std::cout << "Applied new global log level: " << level_to_string(new_level)
              << std::endl;
}

LogConfig::LogLevel DynamicLogConfigManager::level_from_string(
    const std::string& level_str) const {
    if (level_str == "trace") return LogConfig::LogLevel::TRACE;
    if (level_str == "debug") return LogConfig::LogLevel::DEBUG;
    if (level_str == "info") return LogConfig::LogLevel::INFO;
    if (level_str == "warn") return LogConfig::LogLevel::WARN;
    if (level_str == "error") return LogConfig::LogLevel::ERROR;
    if (level_str == "fatal") return LogConfig::LogLevel::FATAL;
    return LogConfig::LogLevel::INFO;
}

std::string DynamicLogConfigManager::level_to_string(
    LogConfig::LogLevel level) const {
    switch (level) {
        case LogConfig::LogLevel::TRACE:
            return "trace";
        case LogConfig::LogLevel::DEBUG:
            return "debug";
        case LogConfig::LogLevel::INFO:
            return "info";
        case LogConfig::LogLevel::WARN:
            return "warn";
        case LogConfig::LogLevel::ERROR:
            return "error";
        case LogConfig::LogLevel::FATAL:
            return "fatal";
        default:
            return "info";
    }
}

}  // namespace shield::log