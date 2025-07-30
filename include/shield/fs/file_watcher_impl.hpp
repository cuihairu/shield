#pragma once

#include <filesystem>

#include "shield/fs/file_watcher.hpp"

#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifdef __APPLE__
#include <sys/event.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace shield::fs {

// Linux inotify实现
#ifdef __linux__
class LinuxFileWatcher : public IFileWatcher {
public:
    LinuxFileWatcher();
    ~LinuxFileWatcher() override;

    bool add_file(const std::string& file_path) override;
    bool remove_file(const std::string& file_path) override;
    bool start() override;
    void stop() override;
    void set_event_handler(FileEventHandler handler) override;
    bool is_supported() const override;
    std::vector<std::string> get_watched_files() const override;
    bool is_running() const override;

private:
    void watch_loop();
    void process_events(const char* buffer, ssize_t length);

    int inotify_fd_;
    std::unordered_map<int, std::string> watch_descriptors_;
    std::unordered_map<std::string, int> file_to_wd_;
    std::atomic<bool> running_;
    std::thread watch_thread_;
    FileEventHandler handler_;
    mutable std::mutex mutex_;
};
#endif

// macOS kqueue实现
#ifdef __APPLE__
class MacOSFileWatcher : public IFileWatcher {
public:
    MacOSFileWatcher();
    ~MacOSFileWatcher() override;

    bool add_file(const std::string& file_path) override;
    bool remove_file(const std::string& file_path) override;
    bool start() override;
    void stop() override;
    void set_event_handler(FileEventHandler handler) override;
    bool is_supported() const override;
    std::vector<std::string> get_watched_files() const override;
    bool is_running() const override;

private:
    void watch_loop();

    int kqueue_fd_;
    std::unordered_map<std::string, int> file_descriptors_;
    std::atomic<bool> running_;
    std::thread watch_thread_;
    FileEventHandler handler_;
    mutable std::mutex mutex_;
};
#endif

// Windows ReadDirectoryChangesW实现
#ifdef _WIN32
class WindowsFileWatcher : public IFileWatcher {
public:
    WindowsFileWatcher();
    ~WindowsFileWatcher() override;

    bool add_file(const std::string& file_path) override;
    bool remove_file(const std::string& file_path) override;
    bool start() override;
    void stop() override;
    void set_event_handler(FileEventHandler handler) override;
    bool is_supported() const override;
    std::vector<std::string> get_watched_files() const override;
    bool is_running() const override;

private:
    struct WatchInfo {
        std::string directory;
        std::vector<std::string> files;
        HANDLE dir_handle;
        OVERLAPPED overlapped;
        char buffer[4096];
        DWORD bytes_returned;
    };

    void watch_loop();
    bool setup_directory_watch(const std::string& directory);
    void process_directory_changes(WatchInfo* watch_info);
    std::string get_directory(const std::string& file_path);

    std::unordered_map<std::string, std::unique_ptr<WatchInfo>> watch_infos_;
    std::atomic<bool> running_;
    std::thread watch_thread_;
    FileEventHandler handler_;
    mutable std::mutex mutex_;
};
#endif

// 轮询实现（通用fallback）
class PollingFileWatcher : public IFileWatcher {
public:
    explicit PollingFileWatcher(
        std::chrono::milliseconds interval = std::chrono::milliseconds(1000));
    ~PollingFileWatcher() override;

    bool add_file(const std::string& file_path) override;
    bool remove_file(const std::string& file_path) override;
    bool start() override;
    void stop() override;
    void set_event_handler(FileEventHandler handler) override;
    bool is_supported() const override { return true; }
    std::vector<std::string> get_watched_files() const override;
    bool is_running() const override;

private:
    struct FileInfo {
        std::string path;
        std::filesystem::file_time_type last_write_time;
        uintmax_t size;
        bool exists;

        FileInfo() : size(0), exists(false) {}
        explicit FileInfo(const std::string& p)
            : path(p), size(0), exists(false) {
            update();
        }

        void update() {
            exists = std::filesystem::exists(path);
            if (exists) {
                try {
                    last_write_time = std::filesystem::last_write_time(path);
                    size = std::filesystem::file_size(path);
                } catch (const std::filesystem::filesystem_error&) {
                    exists = false;
                }
            }
        }

        bool has_changed(const FileInfo& other) const {
            if (exists != other.exists) return true;
            if (!exists) return false;
            return last_write_time != other.last_write_time ||
                   size != other.size;
        }
    };

    void watch_loop();
    void check_file_changes();

    std::unordered_map<std::string, FileInfo> watched_files_;
    std::chrono::milliseconds poll_interval_;
    std::atomic<bool> running_;
    std::thread watch_thread_;
    FileEventHandler handler_;
    mutable std::mutex mutex_;
};

}  // namespace shield::fs