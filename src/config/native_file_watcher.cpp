#include "shield/config/native_file_watcher.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>

#include "shield/log/logger.hpp"

#ifdef __linux__
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <fileapi.h>
#include <windows.h>
#endif

namespace shield::config {

// ============================================================================
// NativeFileWatcher Implementation
// ============================================================================

NativeFileWatcher::NativeFileWatcher() : impl_(create_platform_impl()) {}

NativeFileWatcher::~NativeFileWatcher() { stop(); }

bool NativeFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (std::find(watched_files_.begin(), watched_files_.end(), file_path) !=
        watched_files_.end()) {
        return true;  // Already watching
    }

    if (impl_ && impl_->add_file(file_path)) {
        watched_files_.push_back(file_path);
        SHIELD_LOG_DEBUG << "Added file to native watcher: " << file_path;
        return true;
    }

    return false;
}

bool NativeFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it =
        std::find(watched_files_.begin(), watched_files_.end(), file_path);
    if (it == watched_files_.end()) {
        return true;  // Not watching
    }

    if (impl_ && impl_->remove_file(file_path)) {
        watched_files_.erase(it);
        SHIELD_LOG_DEBUG << "Removed file from native watcher: " << file_path;
        return true;
    }

    return false;
}

bool NativeFileWatcher::start() {
    if (impl_) {
        impl_->set_callback(callback_);
        return impl_->start();
    }
    return false;
}

void NativeFileWatcher::stop() {
    if (impl_) {
        impl_->stop();
    }
}

void NativeFileWatcher::set_callback(FileEventCallback callback) {
    callback_ = std::move(callback);
}

bool NativeFileWatcher::is_native_supported() const {
    return impl_ && impl_->is_supported();
}

std::vector<std::string> NativeFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watched_files_;
}

std::unique_ptr<FileWatcherImpl> NativeFileWatcher::create_platform_impl() {
#ifdef __linux__
    auto impl = std::make_unique<LinuxFileWatcher>();
    if (impl->is_supported()) {
        SHIELD_LOG_INFO << "Using Linux inotify for file watching";
        return impl;
    }
#endif

#ifdef __APPLE__
    auto impl = std::make_unique<MacOSFileWatcher>();
    if (impl->is_supported()) {
        SHIELD_LOG_INFO << "Using macOS kqueue for file watching";
        return impl;
    }
#endif

#ifdef _WIN32
    auto impl = std::make_unique<WindowsFileWatcher>();
    if (impl->is_supported()) {
        SHIELD_LOG_INFO
            << "Using Windows ReadDirectoryChangesW for file watching";
        return impl;
    }
#endif

    SHIELD_LOG_WARN
        << "Native file watching not supported, falling back to polling";
    return std::make_unique<PollingFileWatcher>();
}

// ============================================================================
// Linux inotify Implementation
// ============================================================================

#ifdef __linux__

LinuxFileWatcher::LinuxFileWatcher() : inotify_fd_(-1), running_(false) {
    inotify_fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd_ == -1) {
        SHIELD_LOG_ERROR << "Failed to initialize inotify: " << strerror(errno);
    }
}

LinuxFileWatcher::~LinuxFileWatcher() {
    stop();
    if (inotify_fd_ != -1) {
        close(inotify_fd_);
    }
}

bool LinuxFileWatcher::add_file(const std::string& file_path) {
    if (inotify_fd_ == -1) {
        return false;
    }

    // Remove existing watch if any
    remove_file(file_path);

    int wd = inotify_add_watch(inotify_fd_, file_path.c_str(),
                               IN_MODIFY | IN_MOVED_TO | IN_CREATE | IN_DELETE);

    if (wd == -1) {
        SHIELD_LOG_ERROR << "Failed to add inotify watch for " << file_path
                         << ": " << strerror(errno);
        return false;
    }

    watch_descriptors_[wd] = file_path;
    file_to_wd_[file_path] = wd;
    return true;
}

bool LinuxFileWatcher::remove_file(const std::string& file_path) {
    auto it = file_to_wd_.find(file_path);
    if (it == file_to_wd_.end()) {
        return true;
    }

    int wd = it->second;
    if (inotify_rm_watch(inotify_fd_, wd) == -1) {
        SHIELD_LOG_ERROR << "Failed to remove inotify watch for " << file_path
                         << ": " << strerror(errno);
        return false;
    }

    watch_descriptors_.erase(wd);
    file_to_wd_.erase(it);
    return true;
}

bool LinuxFileWatcher::start() {
    if (inotify_fd_ == -1 || running_.exchange(true)) {
        return false;
    }

    watch_thread_ = std::thread(&LinuxFileWatcher::watch_loop, this);
    return true;
}

void LinuxFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }
}

void LinuxFileWatcher::set_callback(FileEventCallback callback) {
    callback_ = std::move(callback);
}

bool LinuxFileWatcher::is_supported() const { return inotify_fd_ != -1; }

void LinuxFileWatcher::watch_loop() {
    char buffer[4096];
    struct pollfd pfd = {inotify_fd_, POLLIN, 0};

    while (running_) {
        int poll_result = poll(&pfd, 1, 1000);  // 1 second timeout

        if (poll_result > 0 && (pfd.revents & POLLIN)) {
            ssize_t length = read(inotify_fd_, buffer, sizeof(buffer));
            if (length > 0) {
                process_events(buffer, length);
            }
        } else if (poll_result == -1 && errno != EINTR) {
            SHIELD_LOG_ERROR << "Poll error in inotify watch loop: "
                             << strerror(errno);
            break;
        }
    }
}

void LinuxFileWatcher::process_events(const char* buffer, ssize_t length) {
    if (!callback_) return;

    const char* ptr = buffer;
    while (ptr < buffer + length) {
        const struct inotify_event* event =
            reinterpret_cast<const struct inotify_event*>(ptr);

        auto it = watch_descriptors_.find(event->wd);
        if (it != watch_descriptors_.end()) {
            FileEvent file_event;
            file_event.file_path = it->second;

            if (event->mask & IN_MODIFY) {
                file_event.event_type = FileEventType::Modified;
            } else if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                file_event.event_type = FileEventType::Created;
            } else if (event->mask & IN_DELETE) {
                file_event.event_type = FileEventType::Deleted;
            } else {
                ptr += sizeof(struct inotify_event) + event->len;
                continue;
            }

            callback_(file_event);
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
    if (kqueue_fd_ == -1) {
        return false;
    }

    // Remove existing watch if any
    remove_file(file_path);

    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd == -1) {
        SHIELD_LOG_ERROR << "Failed to open file " << file_path << ": "
                         << strerror(errno);
        return false;
    }

    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE, 0, nullptr);

    if (kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr) == -1) {
        SHIELD_LOG_ERROR << "Failed to add kevent for " << file_path << ": "
                         << strerror(errno);
        close(fd);
        return false;
    }

    file_descriptors_[file_path] = fd;
    return true;
}

bool MacOSFileWatcher::remove_file(const std::string& file_path) {
    auto it = file_descriptors_.find(file_path);
    if (it == file_descriptors_.end()) {
        return true;
    }

    int fd = it->second;
    struct kevent change;
    EV_SET(&change, fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);

    close(fd);
    file_descriptors_.erase(it);
    return true;
}

bool MacOSFileWatcher::start() {
    if (kqueue_fd_ == -1 || running_.exchange(true)) {
        return false;
    }

    watch_thread_ = std::thread(&MacOSFileWatcher::watch_loop, this);
    return true;
}

void MacOSFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }
}

void MacOSFileWatcher::set_callback(FileEventCallback callback) {
    callback_ = std::move(callback);
}

bool MacOSFileWatcher::is_supported() const { return kqueue_fd_ != -1; }

void MacOSFileWatcher::watch_loop() {
    struct kevent events[10];
    struct timespec timeout = {1, 0};  // 1 second timeout

    while (running_) {
        int nev = kevent(kqueue_fd_, nullptr, 0, events, 10, &timeout);

        if (nev > 0 && callback_) {
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
                    FileEvent file_event;
                    file_event.file_path = file_path;

                    if (event.fflags &
                        (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB)) {
                        file_event.event_type = FileEventType::Modified;
                    } else if (event.fflags & NOTE_DELETE) {
                        file_event.event_type = FileEventType::Deleted;
                    } else {
                        continue;
                    }

                    callback_(file_event);
                }
            }
        } else if (nev == -1 && errno != EINTR) {
            SHIELD_LOG_ERROR << "kevent error: " << strerror(errno);
            break;
        }
    }
}

#endif  // __APPLE__

// ============================================================================
// Windows ReadDirectoryChangesW Implementation
// ============================================================================

#ifdef _WIN32

WindowsFileWatcher::WindowsFileWatcher() : running_(false) {}

WindowsFileWatcher::~WindowsFileWatcher() { stop(); }

bool WindowsFileWatcher::add_file(const std::string& file_path) {
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
    }

    return true;
}

bool WindowsFileWatcher::remove_file(const std::string& file_path) {
    std::string directory = get_directory(file_path);
    std::string filename = std::filesystem::path(file_path).filename().string();

    auto it = watch_infos_.find(directory);
    if (it == watch_infos_.end()) {
        return true;
    }

    auto& files = it->second->files;
    files.erase(std::remove(files.begin(), files.end(), filename), files.end());

    // If no more files in this directory, remove the watch
    if (files.empty()) {
        CloseHandle(it->second->dir_handle);
        watch_infos_.erase(it);
    }

    return true;
}

bool WindowsFileWatcher::start() {
    if (running_.exchange(true)) {
        return false;
    }

    watch_thread_ = std::thread(&WindowsFileWatcher::watch_loop, this);
    return true;
}

void WindowsFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }

        for (auto& [dir, watch_info] : watch_infos_) {
            CloseHandle(watch_info->dir_handle);
        }
        watch_infos_.clear();
    }
}

void WindowsFileWatcher::set_callback(FileEventCallback callback) {
    callback_ = std::move(callback);
}

bool WindowsFileWatcher::is_supported() const {
    return true;  // Windows API is always available on Windows
}

void WindowsFileWatcher::watch_loop() {
    std::vector<HANDLE> handles;
    std::vector<WatchInfo*> watch_infos_vec;

    while (running_) {
        handles.clear();
        watch_infos_vec.clear();

        for (auto& [dir, watch_info] : watch_infos_) {
            handles.push_back(watch_info->dir_handle);
            watch_infos_vec.push_back(watch_info.get());
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
}

void WindowsFileWatcher::setup_directory_watch(const std::string& directory) {
    auto watch_info = std::make_unique<WatchInfo>();
    watch_info->directory = directory;

    watch_info->dir_handle =
        CreateFileA(directory.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

    if (watch_info->dir_handle == INVALID_HANDLE_VALUE) {
        SHIELD_LOG_ERROR << "Failed to open directory " << directory
                         << " for watching";
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
                         << directory;
        CloseHandle(watch_info->dir_handle);
        CloseHandle(watch_info->overlapped.hEvent);
        return false;
    }

    watch_infos_[directory] = std::move(watch_info);
    return true;
}

void WindowsFileWatcher::process_directory_changes(WatchInfo* watch_info) {
    if (!callback_ || !watch_info) return;

    DWORD bytes_transferred;
    if (!GetOverlappedResult(watch_info->dir_handle, &watch_info->overlapped,
                             &bytes_transferred, FALSE)) {
        return;
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
            FileEvent event;
            event.file_path = watch_info->directory + "\\" + filename;

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

            callback_(event);
        }

    next_entry:
        if (fni->NextEntryOffset == 0) break;
        fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
            reinterpret_cast<char*>(fni) + fni->NextEntryOffset);
    }

    // Restart the watch
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
    : poll_interval_(interval), running_(false) {}

PollingFileWatcher::~PollingFileWatcher() { stop(); }

bool PollingFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (watched_files_.find(file_path) != watched_files_.end()) {
        return true;  // Already watching
    }

    FileInfo info = get_file_info(file_path);
    watched_files_[file_path] = info;
    return true;
}

bool PollingFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    watched_files_.erase(file_path);
    return true;
}

bool PollingFileWatcher::start() {
    if (running_.exchange(true)) {
        return false;
    }

    watch_thread_ = std::thread(&PollingFileWatcher::watch_loop, this);
    return true;
}

void PollingFileWatcher::stop() {
    if (running_.exchange(false)) {
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }
}

void PollingFileWatcher::set_callback(FileEventCallback callback) {
    callback_ = std::move(callback);
}

void PollingFileWatcher::watch_loop() {
    while (running_) {
        check_file_changes();
        std::this_thread::sleep_for(poll_interval_);
    }
}

void PollingFileWatcher::check_file_changes() {
    if (!callback_) return;

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [file_path, old_info] : watched_files_) {
        FileInfo current_info = get_file_info(file_path);

        if (!old_info.exists && current_info.exists) {
            // File created
            FileEvent event{file_path, FileEventType::Created, ""};
            callback_(event);
        } else if (old_info.exists && !current_info.exists) {
            // File deleted
            FileEvent event{file_path, FileEventType::Deleted, ""};
            callback_(event);
        } else if (old_info.exists && current_info.exists) {
            // Check for modifications
            if (old_info.last_write_time != current_info.last_write_time ||
                old_info.size != current_info.size) {
                FileEvent event{file_path, FileEventType::Modified, ""};
                callback_(event);
            }
        }

        old_info = current_info;
    }
}

PollingFileWatcher::FileInfo PollingFileWatcher::get_file_info(
    const std::string& file_path) {
    FileInfo info;
    info.path = file_path;
    info.exists = std::filesystem::exists(file_path);

    if (info.exists) {
        try {
            info.last_write_time = std::filesystem::last_write_time(file_path);
            info.size = std::filesystem::file_size(file_path);
        } catch (const std::filesystem::filesystem_error&) {
            info.exists = false;
        }
    }

    return info;
}

}  // namespace shield::config