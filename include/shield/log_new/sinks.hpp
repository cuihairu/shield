// [SHIELD_LOG] Log sinks
#pragma once

#include "shield/log_new/logger.hpp"

#include <fstream>
#include <memory>
#include <string>
#include <mutex>

namespace shield::log {

/// @brief Console sink (stdout/stderr)
class ConsoleSink : public LogSink {
public:
    ConsoleSink(bool use_stderr = false);
    ~ConsoleSink() override = default;

    void write(const LogRecord& record) override;
    void flush() override;

private:
    bool use_stderr_;
    std::mutex mutex_;
};

/// @brief File sink
class FileSink : public LogSink {
public:
    explicit FileSink(std::string path);
    ~FileSink() override;

    void write(const LogRecord& record) override;
    void flush() override;

private:
    std::string path_;
    std::ofstream file_;
    std::mutex mutex_;
};

/// @brief Rotating file sink
class RotatingFileSink : public LogSink {
public:
    RotatingFileSink(std::string base_path,
                    size_t max_size = 10 * 1024 * 1024,  // 10 MB
                    int max_files = 10);
    ~RotatingFileSink() override;

    void write(const LogRecord& record) override;
    void flush() override;

private:
    void rotate_if_needed();

    std::string base_path_;
    size_t max_size_;
    int max_files_;
    std::ofstream file_;
    size_t current_size_ = 0;
    int current_file_ = 0;
    std::mutex mutex_;
};

/// @brief Create default console sink
std::unique_ptr<ConsoleSink> make_console_sink();

/// @brief Create file sink
std::unique_ptr<FileSink> make_file_sink(std::string path);

/// @brief Create rotating file sink
std::unique_ptr<RotatingFileSink> make_rotating_sink(
    std::string path,
    size_t max_size = 10 * 1024 * 1024,
    int max_files = 10);

}  // namespace shield::log
