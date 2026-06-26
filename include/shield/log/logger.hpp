// [SHIELD_LOG] Logger facade
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace shield::log {

/// @brief Log level
enum class Level { Debug = 0, Info = 1, Warning = 2, Error = 3, Fatal = 4 };

/// @brief Log record
struct LogRecord {
    Level level = Level::Info;
    std::string message;
    std::string logger_name;
    std::string file;
    int line = 0;
    std::string function;
    int64_t timestamp_ms = 0;

    // Service context (if available)
    std::string service_id;
    std::string service_name;
    std::string trace_id;
};

/// @brief Log sink interface
class LogSink {
public:
    virtual ~LogSink() = default;

    /// @brief Write a log record
    virtual void write(const LogRecord& record) = 0;

    /// @brief Flush any buffered output
    virtual void flush() = 0;
};

/// @brief Logger facade
class Logger {
public:
    Logger(std::string name);
    ~Logger();

    // Non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Logging methods
    void debug(std::string_view msg);
    void info(std::string_view msg);
    void warning(std::string_view msg);
    void error(std::string_view msg);
    void fatal(std::string_view msg);

    // With location
    void log(Level level, std::string_view msg, const char* file = nullptr,
             int line = 0, const char* function = nullptr);

    // Set minimum level for this logger
    void set_level(Level level);

    // Get logger name
    const std::string& name() const { return name_; }

    // Global initialization
    static void initialize();
    static void shutdown();

    // Add a sink (global)
    static void add_sink(std::unique_ptr<LogSink> sink);

    // Set global minimum level
    static void set_global_level(Level level);

private:
    std::string name_;
};

/// @brief Get or create a logger
Logger& get_logger(std::string_view name);

/// @brief Set service context for current thread
void set_service_context(std::string service_id, std::string service_name,
                         std::string trace_id);

/// @brief Clear service context
void clear_service_context();

}  // namespace shield::log

// Convenience macros
#define SHIELD_LOG_DEBUG(logger, msg) \
    logger.log(shield::log::Level::Debug, msg, __FILE__, __LINE__, __func__)

#define SHIELD_LOG_INFO(logger, msg) \
    logger.log(shield::log::Level::Info, msg, __FILE__, __LINE__, __func__)

#define SHIELD_LOG_WARNING(logger, msg) \
    logger.log(shield::log::Level::Warning, msg, __FILE__, __LINE__, __func__)

#define SHIELD_LOG_ERROR(logger, msg) \
    logger.log(shield::log::Level::Error, msg, __FILE__, __LINE__, __func__)

#define SHIELD_LOG_FATAL(logger, msg) \
    logger.log(shield::log::Level::Fatal, msg, __FILE__, __LINE__, __func__)
