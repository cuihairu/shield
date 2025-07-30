#include "shield/fs/file_watcher.hpp"

#include <algorithm>
#include <filesystem>

#include "shield/fs/file_watcher_impl.hpp"
#include "shield/log/logger.hpp"

#ifdef __linux__
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#ifdef __APPLE__
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#ifdef _WIN32
#include <fileapi.h>
#include <windows.h>

#include <iostream>
#endif

namespace shield::fs {

// ============================================================================
// FileEventDispatcher Implementation
// ============================================================================

FileEventDispatcher::HandlerId FileEventDispatcher::add_handler(
    FileEventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    HandlerId id = next_id_++;
    handlers_[id] = std::move(handler);
    return id;
}

void FileEventDispatcher::remove_handler(HandlerId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.erase(id);
}

void FileEventDispatcher::dispatch(const FileEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, handler] : handlers_) {
        try {
            handler(event);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Error in file event handler " << id << ": "
                             << e.what();
        }
    }
}

void FileEventDispatcher::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
}

size_t FileEventDispatcher::handler_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

// ============================================================================
// Linux inotify Implementation
// ============================================================================

#ifdef __linux__

LinuxFileWatcher::LinuxFileWatcher() : inotify_fd_(-1), running_(false) {
    inotify_fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd_ == -1) {
        SHIELD_LOG_ERROR << "Failed to initialize inotify: " << strerror(errno);
    } else {
        SHIELD_LOG_DEBUG << "Initialized Linux inotify file watcher";
    }
}

LinuxFileWatcher::~LinuxFileWatcher() {
    stop();
    if (inotify_fd_ != -1) {
        close(inotify_fd_);
    }
}

bool LinuxFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (inotify_fd_ == -1) {
        SHIELD_LOG_ERROR << "inotify not initialized";
        return false;
    }

    // 检查是否已经在监控
    if (file_to_wd_.find(file_path) != file_to_wd_.end()) {
        SHIELD_LOG_DEBUG << "File already being watched: " << file_path;
        return true;
    }

    // 添加监控
    int wd = inotify_add_watch(
        inotify_fd_, file_path.c_str(),
        IN_MODIFY | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_CLOSE_WRITE);

    if (wd == -1) {
        SHIELD_LOG_ERROR << "Failed to add inotify watch for " << file_path
                         << ": " << strerror(errno);
        return false;
    }

    watch_descriptors_[wd] = file_path;
    file_to_wd_[file_path] = wd;

    SHIELD_LOG_DEBUG << "Added inotify watch for: " << file_path
                     << " (wd=" << wd << ")";
    return true;
}

bool LinuxFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_to_wd_.find(file_path);
    if (it == file_to_wd_.end()) {
        return true;  // Not watching
    }

    int wd = it->second;
    if (inotify_rm_watch(inotify_fd_, wd) == -1) {
        SHIELD_LOG_ERROR << "Failed to remove inotify watch for " << file_path
                         << ": " << strerror(errno);
        return false;
    }

    watch_descriptors_.erase(wd);
    file_to_wd_.erase(it);

    SHIELD_LOG_DEBUG << "Removed inotify watch for: " << file_path;
    return true;
}

bool LinuxFileWatcher::start() {
    if (inotify_fd_ == -1) {
        SHIELD_LOG_ERROR << "Cannot start watcher: inotify not initialized";
        return false;
    }

    if (running_.exchange(true)) {
        return false;  // Already running
    }

    watch_thread_ = std::thread(&LinuxFileWatcher::watch_loop, this);
    SHIELD_LOG_INFO << "Started Linux inotify file watcher";
    return true;
}

void LinuxFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
        SHIELD_LOG_INFO << "Stopped Linux inotify file watcher";
    }
}

void LinuxFileWatcher::set_event_handler(FileEventHandler handler) {
    handler_ = std::move(handler);
}

bool LinuxFileWatcher::is_supported() const { return inotify_fd_ != -1; }

std::vector<std::string> LinuxFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> files;
    files.reserve(file_to_wd_.size());
    for (const auto& [path, wd] : file_to_wd_) {
        files.push_back(path);
    }
    return files;
}

bool LinuxFileWatcher::is_running() const { return running_.load(); }

void LinuxFileWatcher::watch_loop() {
    char buffer[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    struct pollfd pfd = {inotify_fd_, POLLIN, 0};

    SHIELD_LOG_DEBUG << "Linux inotify watch loop started";

    while (running_) {
        int poll_result = poll(&pfd, 1, 1000);  // 1 second timeout

        if (poll_result > 0 && (pfd.revents & POLLIN)) {
            ssize_t length = read(inotify_fd_, buffer, sizeof(buffer));
            if (length > 0) {
                process_events(buffer, length);
            } else if (length == -1 && errno != EAGAIN &&
                       errno != EWOULDBLOCK) {
                SHIELD_LOG_ERROR << "Error reading from inotify: "
                                 << strerror(errno);
                break;
            }
        } else if (poll_result == -1 && errno != EINTR) {
            SHIELD_LOG_ERROR << "Poll error in inotify watch loop: "
                             << strerror(errno);
            break;
        }
    }

    SHIELD_LOG_DEBUG << "Linux inotify watch loop ended";
}

void LinuxFileWatcher::process_events(const char* buffer, ssize_t length) {
    if (!handler_) return;

    const char* ptr = buffer;
    while (ptr < buffer + length) {
        const struct inotify_event* event =
            reinterpret_cast<const struct inotify_event*>(ptr);

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = watch_descriptors_.find(event->wd);
        if (it != watch_descriptors_.end()) {
            FileEvent file_event(it->second, FileEventType::Modified);

            if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                file_event.event_type = FileEventType::Modified;
            } else if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                file_event.event_type = FileEventType::Created;
            } else if (event->mask & IN_DELETE) {
                file_event.event_type = FileEventType::Deleted;
            } else {
                ptr += sizeof(struct inotify_event) + event->len;
                continue;
            }

            SHIELD_LOG_DEBUG << "inotify event: " << it->second
                             << " (mask=" << std::hex << event->mask << ")";
            handler_(file_event);
        }

        ptr += sizeof(struct inotify_event) + event->len;
    }
}

#endif  // __linux__

// ============================================================================
// macOS kqueue Implementation
// ============================================================================

#ifdef __APPLE__

MacOSFileWatcher::MacOSFileWatcher() : kqueue_fd_(-1), running_(false) {
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ == -1) {
        SHIELD_LOG_ERROR << "Failed to create kqueue: " << strerror(errno);
    } else {
        SHIELD_LOG_DEBUG << "Initialized macOS kqueue file watcher";
    }
}

MacOSFileWatcher::~MacOSFileWatcher() {
    stop();

    // Close all file descriptors
    for (const auto& [path, fd] : file_descriptors_) {
        close(fd);
    }

    if (kqueue_fd_ != -1) {
        close(kqueue_fd_);
    }
}

bool MacOSFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (kqueue_fd_ == -1) {
        SHIELD_LOG_ERROR << "kqueue not initialized";
        return false;
    }

    // 检查是否已经在监控
    if (file_descriptors_.find(file_path) != file_descriptors_.end()) {
        SHIELD_LOG_DEBUG << "File already being watched: " << file_path;
        return true;
    }

    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd == -1) {
        SHIELD_LOG_ERROR << "Failed to open file " << file_path << ": "
                         << strerror(errno);
        return false;
    }

    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_REVOKE,
           0, nullptr);

    if (kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr) == -1) {
        SHIELD_LOG_ERROR << "Failed to add kevent for " << file_path << ": "
                         << strerror(errno);
        close(fd);
        return false;
    }

    file_descriptors_[file_path] = fd;
    SHIELD_LOG_DEBUG << "Added kqueue watch for: " << file_path << " (fd=" << fd
                     << ")";
    return true;
}

bool MacOSFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_descriptors_.find(file_path);
    if (it == file_descriptors_.end()) {
        return true;  // Not watching
    }

    int fd = it->second;
    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);

    close(fd);
    file_descriptors_.erase(it);

    SHIELD_LOG_DEBUG << "Removed kqueue watch for: " << file_path;
    return true;
}

bool MacOSFileWatcher::start() {
    if (kqueue_fd_ == -1) {
        SHIELD_LOG_ERROR << "Cannot start watcher: kqueue not initialized";
        return false;
    }

    if (running_.exchange(true)) {
        return false;  // Already running
    }

    watch_thread_ = std::thread(&MacOSFileWatcher::watch_loop, this);
    SHIELD_LOG_INFO << "Started macOS kqueue file watcher";
    return true;
}

void MacOSFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
        SHIELD_LOG_INFO << "Stopped macOS kqueue file watcher";
    }
}

void MacOSFileWatcher::set_event_handler(FileEventHandler handler) {
    handler_ = std::move(handler);
}

bool MacOSFileWatcher::is_supported() const { return kqueue_fd_ != -1; }

std::vector<std::string> MacOSFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> files;
    files.reserve(file_descriptors_.size());
    for (const auto& [path, fd] : file_descriptors_) {
        files.push_back(path);
    }
    return files;
}

bool MacOSFileWatcher::is_running() const { return running_.load(); }

void MacOSFileWatcher::watch_loop() {
    struct kevent events[10];
    struct timespec timeout = {1, 0};  // 1 second timeout

    SHIELD_LOG_DEBUG << "macOS kqueue watch loop started";

    while (running_) {
        int nev = kevent(kqueue_fd_, nullptr, 0, events, 10, &timeout);

        if (nev > 0 && handler_) {
            std::lock_guard<std::mutex> lock(mutex_);

            for (int i = 0; i < nev; ++i) {
                const struct kevent& event = events[i];

                // Find file path by file descriptor
                std::string file_path;
                for (const auto& [path, fd] : file_descriptors_) {
                    if (fd == static_cast<int>(event.ident)) {
                        file_path = path;
                        break;
                    }
                }

                if (!file_path.empty()) {
                    FileEvent file_event(file_path, FileEventType::Modified);

                    if (event.fflags &
                        (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB)) {
                        file_event.event_type = FileEventType::Modified;
                    } else if (event.fflags & NOTE_DELETE) {
                        file_event.event_type = FileEventType::Deleted;
                    } else if (event.fflags & NOTE_REVOKE) {
                        file_event.event_type = FileEventType::Deleted;
                    } else {
                        continue;
                    }

                    SHIELD_LOG_DEBUG << "kqueue event: " << file_path
                                     << " (fflags=" << std::hex << event.fflags
                                     << ")";
                    handler_(file_event);
                }
            }
        } else if (nev == -1 && errno != EINTR) {
            SHIELD_LOG_ERROR << "kevent error: " << strerror(errno);
            break;
        }
    }

    SHIELD_LOG_DEBUG << "macOS kqueue watch loop ended";
}

#endif  // __APPLE__

// ============================================================================
// Windows ReadDirectoryChangesW Implementation
// ============================================================================

#ifdef _WIN32

WindowsFileWatcher::WindowsFileWatcher() : running_(false) {
    SHIELD_LOG_DEBUG
        << "Initialized Windows ReadDirectoryChangesW file watcher";
}

WindowsFileWatcher::~WindowsFileWatcher() { stop(); }

bool WindowsFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string directory = get_directory(file_path);
    std::string filename = std::filesystem::path(file_path).filename().string();

    auto it = watch_infos_.find(directory);
    if (it == watch_infos_.end()) {
        if (!setup_directory_watch(directory)) {
            return false;
        }
        it = watch_infos_.find(directory);
    }

    // Add file to the directory's watch list
    auto& files = it->second->files;
    if (std::find(files.begin(), files.end(), filename) == files.end()) {
        files.push_back(filename);
        SHIELD_LOG_DEBUG << "Added Windows watch for: " << file_path;
    }

    return true;
}

bool WindowsFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string directory = get_directory(file_path);
    std::string filename = std::filesystem::path(file_path).filename().string();

    auto it = watch_infos_.find(directory);
    if (it == watch_infos_.end()) {
        return true;  // Not watching
    }

    auto& files = it->second->files;
    files.erase(std::remove(files.begin(), files.end(), filename), files.end());

    // If no more files in this directory, remove the watch
    if (files.empty()) {
        CloseHandle(it->second->dir_handle);
        CloseHandle(it->second->overlapped.hEvent);
        watch_infos_.erase(it);
        SHIELD_LOG_DEBUG << "Removed Windows directory watch for: "
                         << directory;
    } else {
        SHIELD_LOG_DEBUG << "Removed Windows file watch for: " << file_path;
    }

    return true;
}

bool WindowsFileWatcher::start() {
    if (running_.exchange(true)) {
        return false;  // Already running
    }

    watch_thread_ = std::thread(&WindowsFileWatcher::watch_loop, this);
    SHIELD_LOG_INFO << "Started Windows ReadDirectoryChangesW file watcher";
    return true;
}

void WindowsFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [dir, watch_info] : watch_infos_) {
            CloseHandle(watch_info->dir_handle);
            CloseHandle(watch_info->overlapped.hEvent);
        }
        watch_infos_.clear();
        SHIELD_LOG_INFO << "Stopped Windows ReadDirectoryChangesW file watcher";
    }
}

void WindowsFileWatcher::set_event_handler(FileEventHandler handler) {
    handler_ = std::move(handler);
}

bool WindowsFileWatcher::is_supported() const {
    return true;  // Windows API is always available on Windows
}

std::vector<std::string> WindowsFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> files;

    for (const auto& [dir, watch_info] : watch_infos_) {
        for (const auto& filename : watch_info->files) {
            files.push_back(dir + "\\" + filename);
        }
    }

    return files;
}

bool WindowsFileWatcher::is_running() const { return running_.load(); }

void WindowsFileWatcher::watch_loop() {
    SHIELD_LOG_DEBUG << "Windows ReadDirectoryChangesW watch loop started";

    while (running_) {
        std::vector<HANDLE> handles;
        std::vector<WatchInfo*> watch_infos_vec;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            handles.reserve(watch_infos_.size());
            watch_infos_vec.reserve(watch_infos_.size());

            for (auto& [dir, watch_info] : watch_infos_) {
                handles.push_back(watch_info->overlapped.hEvent);
                watch_infos_vec.push_back(watch_info.get());
            }
        }

        if (handles.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        DWORD wait_result = WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()), handles.data(), FALSE,
            1000  // 1 second timeout
        );

        if (wait_result >= WAIT_OBJECT_0 &&
            wait_result < WAIT_OBJECT_0 + handles.size()) {
            DWORD index = wait_result - WAIT_OBJECT_0;
            process_directory_changes(watch_infos_vec[index]);
        }
    }

    SHIELD_LOG_DEBUG << "Windows ReadDirectoryChangesW watch loop ended";
}

bool WindowsFileWatcher::setup_directory_watch(const std::string& directory) {
    auto watch_info = std::make_unique<WatchInfo>();
    watch_info->directory = directory;

    std::wstring wide_dir(directory.begin(), directory.end());
    watch_info->dir_handle =
        CreateFileW(wide_dir.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

    if (watch_info->dir_handle == INVALID_HANDLE_VALUE) {
        SHIELD_LOG_ERROR << "Failed to open directory " << directory
                         << " for watching: " << GetLastError();
        return false;
    }

    ZeroMemory(&watch_info->overlapped, sizeof(OVERLAPPED));
    watch_info->overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    BOOL result = ReadDirectoryChangesW(
        watch_info->dir_handle, watch_info->buffer, sizeof(watch_info->buffer),
        FALSE,  // Don't watch subdirectories
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
            FILE_NOTIFY_CHANGE_FILE_NAME,
        &watch_info->bytes_returned, &watch_info->overlapped, nullptr);

    if (!result) {
        SHIELD_LOG_ERROR << "Failed to start directory watching for "
                         << directory << ": " << GetLastError();
        CloseHandle(watch_info->dir_handle);
        CloseHandle(watch_info->overlapped.hEvent);
        return false;
    }

    watch_infos_[directory] = std::move(watch_info);
    SHIELD_LOG_DEBUG << "Setup Windows directory watch for: " << directory;
    return true;
}

void WindowsFileWatcher::process_directory_changes(WatchInfo* watch_info) {
    if (!handler_ || !watch_info) return;

    DWORD bytes_transferred;
    if (!GetOverlappedResult(watch_info->dir_handle, &watch_info->overlapped,
                             &bytes_transferred, FALSE)) {
        SHIELD_LOG_ERROR << "GetOverlappedResult failed: " << GetLastError();
        return;
    }

    if (bytes_transferred == 0) {
        goto restart_watch;
    }

    FILE_NOTIFY_INFORMATION* fni =
        reinterpret_cast<FILE_NOTIFY_INFORMATION*>(watch_info->buffer);

    while (true) {
        std::wstring wide_filename(fni->FileName,
                                   fni->FileNameLength / sizeof(WCHAR));
        std::string filename(wide_filename.begin(), wide_filename.end());

        // Check if this file is in our watch list
        if (std::find(watch_info->files.begin(), watch_info->files.end(),
                      filename) != watch_info->files.end()) {
            FileEvent event(watch_info->directory + "\\" + filename,
                            FileEventType::Modified);

            switch (fni->Action) {
                case FILE_ACTION_MODIFIED:
                    event.event_type = FileEventType::Modified;
                    break;
                case FILE_ACTION_ADDED:
                case FILE_ACTION_RENAMED_NEW_NAME:
                    event.event_type = FileEventType::Created;
                    break;
                case FILE_ACTION_REMOVED:
                case FILE_ACTION_RENAMED_OLD_NAME:
                    event.event_type = FileEventType::Deleted;
                    break;
                default:
                    goto next_entry;
            }

            SHIELD_LOG_DEBUG << "Windows file event: " << event.file_path
                             << " (action=" << fni->Action << ")";
            handler_(event);
        }

    next_entry:
        if (fni->NextEntryOffset == 0) break;
        fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
            reinterpret_cast<char*>(fni) + fni->NextEntryOffset);
    }

restart_watch:
    // Restart the watch
    ResetEvent(watch_info->overlapped.hEvent);
    ReadDirectoryChangesW(
        watch_info->dir_handle, watch_info->buffer, sizeof(watch_info->buffer),
        FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
            FILE_NOTIFY_CHANGE_FILE_NAME,
        &watch_info->bytes_returned, &watch_info->overlapped, nullptr);
}

std::string WindowsFileWatcher::get_directory(const std::string& file_path) {
    return std::filesystem::path(file_path).parent_path().string();
}

#endif  // _WIN32

// ============================================================================
// Polling Implementation (Fallback)
// ============================================================================

PollingFileWatcher::PollingFileWatcher(std::chrono::milliseconds interval)
    : poll_interval_(interval), running_(false) {
    SHIELD_LOG_DEBUG << "Initialized polling file watcher with interval: "
                     << interval.count() << "ms";
}

PollingFileWatcher::~PollingFileWatcher() { stop(); }

bool PollingFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (watched_files_.find(file_path) != watched_files_.end()) {
        SHIELD_LOG_DEBUG << "File already being watched: " << file_path;
        return true;  // Already watching
    }

    watched_files_[file_path] = FileInfo(file_path);
    SHIELD_LOG_DEBUG << "Added file to polling watcher: " << file_path;
    return true;
}

bool PollingFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto erased = watched_files_.erase(file_path);
    if (erased > 0) {
        SHIELD_LOG_DEBUG << "Removed file from polling watcher: " << file_path;
    }
    return true;
}

bool PollingFileWatcher::start() {
    if (running_.exchange(true)) {
        return false;  // Already running
    }

    watch_thread_ = std::thread(&PollingFileWatcher::watch_loop, this);
    SHIELD_LOG_INFO << "Started polling file watcher";
    return true;
}

void PollingFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
        SHIELD_LOG_INFO << "Stopped polling file watcher";
    }
}

void PollingFileWatcher::set_event_handler(FileEventHandler handler) {
    handler_ = std::move(handler);
}

std::vector<std::string> PollingFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> files;
    files.reserve(watched_files_.size());
    for (const auto& [path, info] : watched_files_) {
        files.push_back(path);
    }
    return files;
}

bool PollingFileWatcher::is_running() const { return running_.load(); }

void PollingFileWatcher::watch_loop() {
    SHIELD_LOG_DEBUG << "Polling file watcher loop started";

    while (running_) {
        check_file_changes();
        std::this_thread::sleep_for(poll_interval_);
    }

    SHIELD_LOG_DEBUG << "Polling file watcher loop ended";
}

void PollingFileWatcher::check_file_changes() {
    if (!handler_) return;

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [file_path, old_info] : watched_files_) {
        FileInfo current_info(file_path);

        if (!old_info.exists && current_info.exists) {
            // File created
            FileEvent event(file_path, FileEventType::Created);
            SHIELD_LOG_DEBUG << "Polling detected file created: " << file_path;
            handler_(event);
        } else if (old_info.exists && !current_info.exists) {
            // File deleted
            FileEvent event(file_path, FileEventType::Deleted);
            SHIELD_LOG_DEBUG << "Polling detected file deleted: " << file_path;
            handler_(event);
        } else if (old_info.exists && current_info.exists &&
                   old_info.has_changed(current_info)) {
            // File modified
            FileEvent event(file_path, FileEventType::Modified);
            SHIELD_LOG_DEBUG << "Polling detected file modified: " << file_path;
            handler_(event);
        }

        old_info = current_info;
    }
}

// ============================================================================
// FileWatchManager Implementation
// ============================================================================

FileWatchManager& FileWatchManager::instance() {
    static FileWatchManager instance;
    return instance;
}

std::shared_ptr<IFileWatcher> FileWatchManager::create_watcher(
    const std::string& name, std::chrono::milliseconds poll_interval) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (watchers_.find(name) != watchers_.end()) {
        SHIELD_LOG_WARN << "File watcher '" << name << "' already exists";
        return watchers_[name];
    }

    auto watcher = FileWatcherFactory::create_best_watcher(poll_interval);
    auto shared_watcher = std::shared_ptr<IFileWatcher>(watcher.release());
    watchers_[name] = shared_watcher;

    SHIELD_LOG_INFO << "Created file watcher: " << name;
    return shared_watcher;
}

std::shared_ptr<IFileWatcher> FileWatchManager::get_watcher(
    const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = watchers_.find(name);
    return (it != watchers_.end()) ? it->second : nullptr;
}

void FileWatchManager::remove_watcher(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = watchers_.find(name);
    if (it != watchers_.end()) {
        it->second->stop();
        watchers_.erase(it);
        SHIELD_LOG_INFO << "Removed file watcher: " << name;
    }
}

std::vector<std::string> FileWatchManager::get_watcher_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(watchers_.size());
    for (const auto& [name, watcher] : watchers_) {
        names.push_back(name);
    }
    return names;
}

void FileWatchManager::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, watcher] : watchers_) {
        watcher->stop();
    }
    SHIELD_LOG_INFO << "Stopped all file watchers (" << watchers_.size() << ")";
}

void FileWatchManager::start_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, watcher] : watchers_) {
        if (!watcher->is_running()) {
            watcher->start();
        }
    }
    SHIELD_LOG_INFO << "Started all file watchers (" << watchers_.size() << ")";
}

// ============================================================================
// FileWatcherFactory Implementation
// ============================================================================

std::unique_ptr<IFileWatcher> FileWatcherFactory::create_best_watcher(
    std::chrono::milliseconds poll_interval) {
    // 尝试创建原生监控器
    auto native = create_native_watcher();
    if (native && native->is_supported()) {
        SHIELD_LOG_INFO << "Using native file watcher";
        return native;
    }

    // 降级到轮询模式
    SHIELD_LOG_INFO << "Using polling file watcher with interval: "
                    << poll_interval.count() << "ms";
    return create_polling_watcher(poll_interval);
}

std::unique_ptr<IFileWatcher> FileWatcherFactory::create_polling_watcher(
    std::chrono::milliseconds poll_interval) {
    return std::make_unique<PollingFileWatcher>(poll_interval);
}

std::unique_ptr<IFileWatcher> FileWatcherFactory::create_native_watcher() {
#ifdef __linux__
    return std::make_unique<LinuxFileWatcher>();
#elif __APPLE__
    return std::make_unique<MacOSFileWatcher>();
#elif _WIN32
    return std::make_unique<WindowsFileWatcher>();
#else
    return nullptr;
#endif
}

bool FileWatcherFactory::is_native_supported() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

// ============================================================================
// FileWatcher Implementation
// ============================================================================

FileWatcher::FileWatcher(std::chrono::milliseconds poll_interval)
    : impl_(FileWatcherFactory::create_best_watcher(poll_interval)),
      dispatcher_(std::make_unique<FileEventDispatcher>()) {
    // 设置分发器作为事件处理器
    impl_->set_event_handler(
        [this](const FileEvent& event) { dispatcher_->dispatch(event); });
}

FileWatcher::FileWatcher(std::shared_ptr<IFileWatcher> impl)
    : impl_(std::move(impl)),
      dispatcher_(std::make_unique<FileEventDispatcher>()) {
    impl_->set_event_handler(
        [this](const FileEvent& event) { dispatcher_->dispatch(event); });
}

FileWatcher::~FileWatcher() {
    if (impl_) {
        impl_->stop();
    }
}

bool FileWatcher::add_file(const std::string& file_path) {
    return impl_ ? impl_->add_file(file_path) : false;
}

bool FileWatcher::remove_file(const std::string& file_path) {
    return impl_ ? impl_->remove_file(file_path) : false;
}

void FileWatcher::start() {
    if (impl_) {
        impl_->start();
    }
}

void FileWatcher::stop() {
    if (impl_) {
        impl_->stop();
    }
}

FileEventDispatcher::HandlerId FileWatcher::add_handler(
    FileEventHandler handler) {
    return dispatcher_->add_handler(std::move(handler));
}

void FileWatcher::remove_handler(FileEventDispatcher::HandlerId id) {
    dispatcher_->remove_handler(id);
}

bool FileWatcher::is_running() const {
    return impl_ ? impl_->is_running() : false;
}

bool FileWatcher::is_native_supported() const {
    return impl_ ? impl_->is_supported() : false;
}

std::vector<std::string> FileWatcher::get_watched_files() const {
    return impl_ ? impl_->get_watched_files() : std::vector<std::string>{};
}

}  // namespace shield::fs