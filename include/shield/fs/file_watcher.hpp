#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace shield::fs {

// File event type
enum class FileEventType {
    Modified,  // File modified
    Created,   // File created
    Deleted,   // File deleted
    Moved      // File moved/renamed
};

// 输出流操作符
inline std::ostream& operator<<(std::ostream& os, FileEventType type) {
    switch (type) {
        case FileEventType::Modified:
            return os << "Modified";
        case FileEventType::Created:
            return os << "Created";
        case FileEventType::Deleted:
            return os << "Deleted";
        case FileEventType::Moved:
            return os << "Moved";
        default:
            return os << "Unknown";
    }
}

// File event
struct FileEvent {
    std::string file_path;     // File path where event occurred
    FileEventType event_type;  // Event type
    std::string old_path;      // Original path for move events
    std::chrono::system_clock::time_point timestamp;  // Event timestamp

    FileEvent(const std::string& path, FileEventType type,
              const std::string& old_p = "")
        : file_path(path),
          event_type(type),
          old_path(old_p),
          timestamp(std::chrono::system_clock::now()) {}
};

// File event handler

using FileEventHandler = std::function<void(const FileEvent&)>;

// File monitoring interface
class IFileWatcher {
public:
    virtual ~IFileWatcher() = default;

    // Add file monitoring
    virtual bool add_file(const std::string& file_path) = 0;

    // Remove file monitoring
    virtual bool remove_file(const std::string& file_path) = 0;

    // Start monitoring
    virtual bool start() = 0;

    // Stop monitoring
    virtual void stop() = 0;

    // Set event handler
    virtual void set_event_handler(FileEventHandler handler) = 0;

    // Check if current platform is supported
    virtual bool is_supported() const = 0;

    // Get list of monitored files
    virtual std::vector<std::string> get_watched_files() const = 0;

    // Check if running
    virtual bool is_running() const = 0;
};

// File watcher factory
class FileWatcherFactory {
public:
    // Create the best file watcher (prefer native API)
    static std::unique_ptr<IFileWatcher> create_best_watcher(
        std::chrono::milliseconds poll_interval =
            std::chrono::milliseconds(1000));

    // Create polling watcher
    static std::unique_ptr<IFileWatcher> create_polling_watcher(
        std::chrono::milliseconds poll_interval =
            std::chrono::milliseconds(1000));

    // Create native watcher (if supported)
    static std::unique_ptr<IFileWatcher> create_native_watcher();

    // Check if native watcher is available
    static bool is_native_supported();
};

// Event dispatcher - supports multiple handlers
class FileEventDispatcher {
public:
    using HandlerId = size_t;

    // Add event handler
    HandlerId add_handler(FileEventHandler handler);

    // Remove event handler
    void remove_handler(HandlerId id);

    // Dispatch event to all handlers
    void dispatch(const FileEvent& event);

    // Clear all handlers
    void clear();

    // Get handler count
    size_t handler_count() const;

private:
    std::unordered_map<HandlerId, FileEventHandler> handlers_;
    HandlerId next_id_ = 1;
    mutable std::mutex mutex_;
};

// File watch manager - global singleton
class FileWatchManager {
public:
    static FileWatchManager& instance();

    // Create new watcher
    std::shared_ptr<IFileWatcher> create_watcher(
        const std::string& name, std::chrono::milliseconds poll_interval =
                                     std::chrono::milliseconds(1000));

    // Get existing watcher
    std::shared_ptr<IFileWatcher> get_watcher(const std::string& name);

    // Remove watcher
    void remove_watcher(const std::string& name);

    // Get all watcher names
    std::vector<std::string> get_watcher_names() const;

    // Stop all watchers
    void stop_all();

    // Start all watchers
    void start_all();

private:
    FileWatchManager() = default;
    std::unordered_map<std::string, std::shared_ptr<IFileWatcher>> watchers_;
    mutable std::mutex mutex_;
};

// Convenient file watcher wrapper class
class FileWatcher {
public:
    explicit FileWatcher(std::chrono::milliseconds poll_interval =
                             std::chrono::milliseconds(1000));
    explicit FileWatcher(std::shared_ptr<IFileWatcher> impl);
    ~FileWatcher();

    // Basic operations
    bool add_file(const std::string& file_path);
    bool remove_file(const std::string& file_path);
    void start();
    void stop();

    // Event handling
    FileEventDispatcher::HandlerId add_handler(FileEventHandler handler);
    void remove_handler(FileEventDispatcher::HandlerId id);

    // Status queries
    bool is_running() const;
    bool is_native_supported() const;
    std::vector<std::string> get_watched_files() const;

    // Get underlying implementation
    std::shared_ptr<IFileWatcher> get_impl() const { return impl_; }

private:
    std::shared_ptr<IFileWatcher> impl_;
    std::unique_ptr<FileEventDispatcher> dispatcher_;
};

}  // namespace shield::fs