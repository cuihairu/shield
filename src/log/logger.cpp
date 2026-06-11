// [SHIELD_LOG] Logger implementation
#include "shield/log_new/logger.hpp"
#include "shield/log_new/sinks.hpp"

#include "shield/base/time.hpp"

#include <iostream>
#include <shared_mutex>
#include <unordered_map>

namespace shield::log {

// Global logger state
struct GlobalState {
    std::vector<std::unique_ptr<LogSink>> sinks;
    Level global_level = Level::Info;
    std::shared_mutex mutex;
    bool initialized = false;
};

static GlobalState* g_state = nullptr;
static std::unique_ptr<GlobalState> g_state_owner;

// Thread-local service context
thread_local LogRecord g_service_context{};

// Level helpers
const char* level_to_string(Level level) {
    switch (level) {
        case Level::Debug:   return "DEBUG";
        case Level::Info:    return "INFO";
        case Level::Warning: return "WARN";
        case Level::Error:   return "ERROR";
        case Level::Fatal:   return "FATAL";
        default:            return "UNKNOWN";
    }
}

// ConsoleSink implementation
ConsoleSink::ConsoleSink(bool use_stderr)
    : use_stderr_(use_stderr) {}

void ConsoleSink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);

    auto& stream = use_stderr_ ? std::cerr : std::cout;

    // Format: [timestamp] [level] [service] message
    stream << "[" << record.timestamp_ms << "] "
           << "[" << level_to_string(record.level) << "]";

    if (!record.service_name.empty()) {
        stream << " [" << record.service_name << "]";
    }

    stream << " " << record.message;

    if (!record.file.empty()) {
        stream << " (" << record.file << ":" << record.line << ")";
    }

    stream << std::endl;
}

void ConsoleSink::flush() {
    (use_stderr_ ? std::cerr : std::cout).flush();
}

// FileSink implementation
FileSink::FileSink(std::string path)
    : path_(std::move(path)) {
    file_.open(path_, std::ios::out | std::ios::app);
}

FileSink::~FileSink() {
    if (file_.is_open()) {
        file_.close();
    }
}

void FileSink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);

    if (!file_.is_open()) return;

    // Format: [timestamp] [level] [service] message
    file_ << "[" << record.timestamp_ms << "] "
          << "[" << level_to_string(record.level) << "]";

    if (!record.service_name.empty()) {
        file_ << " [" << record.service_name << "]";
    }

    file_ << " " << record.message << std::endl;
}

void FileSink::flush() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

// RotatingFileSink implementation
RotatingFileSink::RotatingFileSink(std::string base_path,
                                  size_t max_size,
                                  int max_files)
    : base_path_(std::move(base_path)),
      max_size_(max_size),
      max_files_(max_files) {
    // Open the current file
    std::string path = base_path_;
    file_.open(path, std::ios::out | std::ios::app);

    // Get current size
    if (file_.is_open()) {
        file_.seekp(0, std::ios::end);
        current_size_ = file_.tellp();
    }
}

RotatingFileSink::~RotatingFileSink() {
    if (file_.is_open()) {
        file_.close();
    }
}

void RotatingFileSink::rotate_if_needed() {
    if (current_size_ < max_size_) return;

    file_.close();

    // Rotate files
    for (int i = max_files_ - 1; i >= 1; --i) {
        std::string old_name = base_path_ + "." + std::to_string(i);
        std::string new_name = base_path_ + "." + std::to_string(i + 1);

        if (i == max_files_ - 1) {
            // Delete the oldest file
            std::remove(old_name.c_str());
        } else {
            std::rename(old_name.c_str(), new_name.c_str());
        }
    }

    // Move current to .1
    std::rename(base_path_.c_str(), (base_path_ + ".1").c_str());

    // Open new file
    file_.open(base_path_, std::ios::out | std::ios::app);
    current_size_ = 0;
}

void RotatingFileSink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);

    rotate_if_needed();

    if (!file_.is_open()) return;

    std::string log_line = "[" + std::to_string(record.timestamp_ms) + "] " +
                          "[" + level_to_string(record.level) + "] " +
                          record.message + "\n";

    file_ << log_line;
    current_size_ += log_line.length();
}

void RotatingFileSink::flush() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

// Factory functions
std::unique_ptr<ConsoleSink> make_console_sink() {
    return std::make_unique<ConsoleSink>();
}

std::unique_ptr<FileSink> make_file_sink(std::string path) {
    return std::make_unique<FileSink>(std::move(path));
}

std::unique_ptr<RotatingFileSink> make_rotating_sink(
    std::string path,
    size_t max_size,
    int max_files) {
    return std::make_unique<RotatingFileSink>(
        std::move(path), max_size, max_files);
}

// Logger implementation
struct Logger::Impl {
    std::string name;
    Level level = Level::Info;  // Per-logger level (not implemented yet)
};

Logger::Logger(std::string name)
    : name_(std::move(name)),
      impl_(std::make_unique<Impl>()) {
    impl_->name = name_;
}

Logger::~Logger() = default;

void Logger::debug(std::string_view msg) {
    log(Level::Debug, msg);
}

void Logger::info(std::string_view msg) {
    log(Level::Info, msg);
}

void Logger::warning(std::string_view msg) {
    log(Level::Warning, msg);
}

void Logger::error(std::string_view msg) {
    log(Level::Error, msg);
}

void Logger::fatal(std::string_view msg) {
    log(Level::Fatal, msg);
}

void Logger::log(Level level, std::string_view msg,
                 const char* file, int line, const char* function) {
    if (!g_state) return;

    // Check global level
    std::shared_lock lock(g_state->mutex);
    if (level < g_state->global_level) return;

    // Create log record
    LogRecord record;
    record.level = level;
    record.message = std::string(msg);
    record.logger_name = name_;
    record.file = file ? file : "";
    record.line = line;
    record.function = function ? function : "";
    record.timestamp_ms = base::now_ms();

    // Add service context if available
    record.service_id = g_service_context.service_id;
    record.service_name = g_service_context.service_name;
    record.trace_id = g_service_context.trace_id;

    // Write to all sinks
    for (auto& sink : g_state->sinks) {
        sink->write(record);
    }
}

void Logger::set_level(Level level) {
    impl_->level = level;
}

// Global functions
void Logger::initialize() {
    if (g_state) return;

    g_state_owner = std::make_unique<GlobalState>();
    g_state = g_state_owner.get();
    g_state->initialized = true;

    // Add default console sink
    g_state->sinks.push_back(make_console_sink());
}

void Logger::shutdown() {
    if (!g_state) return;

    std::unique_lock lock(g_state->mutex);
    g_state->sinks.clear();
    g_state->initialized = false;
}

void Logger::add_sink(std::unique_ptr<LogSink> sink) {
    if (!g_state) initialize();

    std::unique_lock lock(g_state->mutex);
    g_state->sinks.push_back(std::move(sink));
}

void Logger::set_global_level(Level level) {
    if (!g_state) initialize();

    std::unique_lock lock(g_state->mutex);
    g_state->global_level = level;
}

Logger& get_logger(std::string_view name) {
    // Simple logger cache (could be improved with a proper registry)
    static std::unordered_map<std::string, std::unique_ptr<Logger>> cache;
    static std::mutex cache_mutex;

    std::string key(name);
    std::lock_guard lock(cache_mutex);

    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, std::make_unique<Logger>(key)).first;
    }

    return *it->second;
}

// Service context
void set_service_context(std::string service_id, std::string service_name,
                        std::string trace_id) {
    g_service_context.service_id = std::move(service_id);
    g_service_context.service_name = std::move(service_name);
    g_service_context.trace_id = std::move(trace_id);
}

void clear_service_context() {
    g_service_context = LogRecord{};
}

}  // namespace shield::log
