// [SHIELD_LOG] Logger implementation
#include "shield/log/logger.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "shield/base/time.hpp"
#include "shield/log/sinks.hpp"

namespace shield::log {

namespace {

struct ServiceContext {
    std::string service_id;
    std::string service_name;
    std::string trace_id;
};

std::mutex g_mutex;
std::unordered_map<std::string, std::unique_ptr<Logger>> g_loggers;
std::vector<std::unique_ptr<LogSink>> g_sinks;
Level g_global_level = Level::Info;
thread_local ServiceContext g_service_context;

const char* level_name(Level level) {
    switch (level) {
        case Level::Debug:
            return "DEBUG";
        case Level::Info:
            return "INFO";
        case Level::Warning:
            return "WARN";
        case Level::Error:
            return "ERROR";
        case Level::Fatal:
            return "FATAL";
    }
    return "INFO";
}

std::ostream& output_stream(bool use_stderr, Level level) {
    if (use_stderr || level >= Level::Error) {
        return std::cerr;
    }
    return std::cout;
}

std::string format_record(const LogRecord& record) {
    std::ostringstream out;
    out << record.timestamp_ms << " [" << level_name(record.level) << "] "
        << record.logger_name << ": " << record.message;

    if (!record.file.empty() && record.line > 0) {
        out << " (" << record.file << ":" << record.line << ")";
    }

    if (!record.trace_id.empty()) {
        out << " trace=" << record.trace_id;
    }

    return out.str();
}

}  // namespace

Logger::Logger(std::string name) : name_(std::move(name)) {}

Logger::~Logger() = default;

void Logger::debug(std::string_view msg) { log(Level::Debug, msg); }

void Logger::info(std::string_view msg) { log(Level::Info, msg); }

void Logger::warning(std::string_view msg) { log(Level::Warning, msg); }

void Logger::error(std::string_view msg) { log(Level::Error, msg); }

void Logger::fatal(std::string_view msg) { log(Level::Fatal, msg); }

void Logger::log(Level level, std::string_view msg, const char* file, int line,
                 const char* function) {
    std::lock_guard lock(g_mutex);

    if (level < g_global_level) {
        return;
    }

    LogRecord record;
    record.level = level;
    record.message = std::string(msg);
    record.logger_name = name_;
    record.file = file ? file : "";
    record.line = line;
    record.function = function ? function : "";
    record.timestamp_ms = shield::base::now_ms();
    record.service_id = g_service_context.service_id;
    record.service_name = g_service_context.service_name;
    record.trace_id = g_service_context.trace_id;

    for (auto& sink : g_sinks) {
        sink->write(record);
    }
}

void Logger::set_level(Level level) { set_global_level(level); }

void Logger::initialize() {
    std::lock_guard lock(g_mutex);
    if (g_sinks.empty()) {
        g_sinks.push_back(make_console_sink());
    }
}

void Logger::shutdown() {
    std::lock_guard lock(g_mutex);
    for (auto& sink : g_sinks) {
        sink->flush();
    }
    g_sinks.clear();
    g_loggers.clear();
}

void Logger::add_sink(std::unique_ptr<LogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard lock(g_mutex);
    g_sinks.push_back(std::move(sink));
}

void Logger::set_global_level(Level level) {
    std::lock_guard lock(g_mutex);
    g_global_level = level;
}

Logger& get_logger(std::string_view name) {
    std::lock_guard lock(g_mutex);

    std::string key(name);
    auto it = g_loggers.find(key);
    if (it != g_loggers.end()) {
        return *it->second;
    }

    auto logger = std::make_unique<Logger>(key);
    auto* logger_ptr = logger.get();
    g_loggers.emplace(std::move(key), std::move(logger));
    return *logger_ptr;
}

void set_service_context(std::string service_id, std::string service_name,
                         std::string trace_id) {
    g_service_context.service_id = std::move(service_id);
    g_service_context.service_name = std::move(service_name);
    g_service_context.trace_id = std::move(trace_id);
}

void clear_service_context() { g_service_context = {}; }

ConsoleSink::ConsoleSink(bool use_stderr) : use_stderr_(use_stderr) {}

void ConsoleSink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    output_stream(use_stderr_, record.level) << format_record(record) << '\n';
}

void ConsoleSink::flush() {
    std::lock_guard lock(mutex_);
    std::cout.flush();
    std::cerr.flush();
}

FileSink::FileSink(std::string path) : path_(std::move(path)) {
    file_.open(path_, std::ios::app);
}

FileSink::~FileSink() { flush(); }

void FileSink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_ << format_record(record) << '\n';
    }
}

void FileSink::flush() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

RotatingFileSink::RotatingFileSink(std::string base_path, size_t max_size,
                                   int max_files)
    : base_path_(std::move(base_path)),
      max_size_(max_size),
      max_files_(std::max(1, max_files)) {
    file_.open(base_path_, std::ios::app);
    if (std::filesystem::exists(base_path_)) {
        current_size_ =
            static_cast<size_t>(std::filesystem::file_size(base_path_));
    }
}

RotatingFileSink::~RotatingFileSink() { flush(); }

void RotatingFileSink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    if (!file_.is_open()) {
        return;
    }

    auto line = format_record(record);
    current_size_ += line.size() + 1;
    rotate_if_needed();
    file_ << line << '\n';
}

void RotatingFileSink::flush() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

void RotatingFileSink::rotate_if_needed() {
    if (current_size_ <= max_size_) {
        return;
    }

    file_.close();
    current_file_ = (current_file_ + 1) % max_files_;
    auto rotated = base_path_ + "." + std::to_string(current_file_);
    std::filesystem::rename(base_path_, rotated);
    file_.open(base_path_, std::ios::trunc);
    current_size_ = 0;
}

std::unique_ptr<ConsoleSink> make_console_sink() {
    return std::make_unique<ConsoleSink>();
}

std::unique_ptr<FileSink> make_file_sink(std::string path) {
    return std::make_unique<FileSink>(std::move(path));
}

std::unique_ptr<RotatingFileSink> make_rotating_sink(std::string path,
                                                     size_t max_size,
                                                     int max_files) {
    return std::make_unique<RotatingFileSink>(std::move(path), max_size,
                                              max_files);
}

}  // namespace shield::log
