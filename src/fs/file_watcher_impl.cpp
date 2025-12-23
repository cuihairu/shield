// shield/src/fs/file_watcher_impl.cpp
#include "shield/fs/file_watcher_impl.hpp"

#include <iostream>
#include <sys/stat.h>

namespace shield::fs {

// =====================================
// FileEventDispatcher 实现
// =====================================

FileEventDispatcher::HandlerId FileEventDispatcher::add_handler(
    FileEventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[next_id_] = std::move(handler);
    return next_id_++;
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
            std::cerr << "[FileEventDispatcher] Handler exception: " << e.what()
                      << std::endl;
        }
    }
}

void FileEventDispatcher::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
    next_id_ = 1;
}

size_t FileEventDispatcher::handler_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

// =====================================
// FileWatcherFactory 实现
// =====================================

std::unique_ptr<IFileWatcher> FileWatcherFactory::create_best_watcher(
    std::chrono::milliseconds poll_interval) {
    if (is_native_supported()) {
        return create_native_watcher();
    }
    return create_polling_watcher(poll_interval);
}

std::unique_ptr<IFileWatcher> FileWatcherFactory::create_polling_watcher(
    std::chrono::milliseconds poll_interval) {
    return std::make_unique<PollingFileWatcher>(poll_interval);
}

std::unique_ptr<IFileWatcher> FileWatcherFactory::create_native_watcher() {
#if defined(__linux__)
    return std::make_unique<LinuxFileWatcher>();
#elif defined(__APPLE__)
    return std::make_unique<MacOSFileWatcher>();
#elif defined(_WIN32)
    return std::make_unique<WindowsFileWatcher>();
#else
    return std::make_unique<PollingFileWatcher>();
#endif
}

bool FileWatcherFactory::is_native_supported() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

// =====================================
// FileWatchManager 实现
// =====================================

FileWatchManager& FileWatchManager::instance() {
    static FileWatchManager instance;
    return instance;
}

std::shared_ptr<IFileWatcher> FileWatchManager::create_watcher(
    const std::string& name, std::chrono::milliseconds poll_interval) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto watcher = FileWatcherFactory::create_best_watcher(poll_interval);
    watchers_[name] = std::shared_ptr<IFileWatcher>(watcher.release());
    return watchers_[name];
}

std::shared_ptr<IFileWatcher> FileWatchManager::get_watcher(
    const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = watchers_.find(name);
    if (it != watchers_.end()) {
        return it->second;
    }
    return nullptr;
}

void FileWatchManager::remove_watcher(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = watchers_.find(name);
    if (it != watchers_.end()) {
        if (it->second) {
            it->second->stop();
        }
        watchers_.erase(it);
    }
}

std::vector<std::string> FileWatchManager::get_watcher_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(watchers_.size());
    for (const auto& [name, _] : watchers_) {
        names.push_back(name);
    }
    return names;
}

void FileWatchManager::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, watcher] : watchers_) {
        if (watcher) {
            watcher->stop();
        }
    }
}

void FileWatchManager::start_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, watcher] : watchers_) {
        if (watcher) {
            watcher->start();
        }
    }
}

// =====================================
// FileWatcher 实现
// =====================================

FileWatcher::FileWatcher(std::chrono::milliseconds poll_interval)
    : impl_(FileWatcherFactory::create_best_watcher(poll_interval)),
      dispatcher_(std::make_unique<FileEventDispatcher>()) {
    if (impl_) {
        impl_->set_event_handler([this](const FileEvent& event) {
            dispatcher_->dispatch(event);
        });
    }
}

FileWatcher::FileWatcher(std::shared_ptr<IFileWatcher> impl)
    : impl_(std::move(impl)),
      dispatcher_(std::make_unique<FileEventDispatcher>()) {
    if (impl_) {
        impl_->set_event_handler([this](const FileEvent& event) {
            dispatcher_->dispatch(event);
        });
    }
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

// =====================================
// PollingFileWatcher 实现
// =====================================

PollingFileWatcher::PollingFileWatcher(std::chrono::milliseconds interval)
    : poll_interval_(interval), running_(false) {}

PollingFileWatcher::~PollingFileWatcher() {
    stop();
}

bool PollingFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (file_path.empty()) {
        return false;
    }

    watched_files_[file_path] = FileInfo(file_path);
    std::cout << "[PollingFileWatcher] Added file: " << file_path << std::endl;
    return true;
}

bool PollingFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = watched_files_.find(file_path);
    if (it != watched_files_.end()) {
        watched_files_.erase(it);
        std::cout << "[PollingFileWatcher] Removed file: " << file_path
                  << std::endl;
        return true;
    }
    return false;
}

bool PollingFileWatcher::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        return false;
    }

    running_ = true;
    watch_thread_ = std::thread(&PollingFileWatcher::watch_loop, this);

    std::cout << "[PollingFileWatcher] Started with interval: "
              << poll_interval_.count() << "ms" << std::endl;
    return true;
}

void PollingFileWatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }

    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }

    std::cout << "[PollingFileWatcher] Stopped" << std::endl;
}

void PollingFileWatcher::set_event_handler(FileEventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

std::vector<std::string> PollingFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> files;
    files.reserve(watched_files_.size());
    for (const auto& [path, _] : watched_files_) {
        files.push_back(path);
    }
    return files;
}

bool PollingFileWatcher::is_running() const {
    return running_;
}

void PollingFileWatcher::watch_loop() {
    std::cout << "[PollingFileWatcher] Watch loop started" << std::endl;

    while (running_) {
        try {
            check_file_changes();
        } catch (const std::exception& e) {
            std::cerr << "[PollingFileWatcher] Error in watch loop: " << e.what()
                      << std::endl;
        }

        std::this_thread::sleep_for(poll_interval_);
    }

    std::cout << "[PollingFileWatcher] Watch loop stopped" << std::endl;
}

void PollingFileWatcher::check_file_changes() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [path, old_info] : watched_files_) {
        FileInfo new_info(path);
        new_info.update();

        if (new_info.has_changed(old_info)) {
            FileEventType event_type = FileEventType::Modified;

            if (!old_info.exists && new_info.exists) {
                event_type = FileEventType::Created;
            } else if (old_info.exists && !new_info.exists) {
                event_type = FileEventType::Deleted;
            }

            FileEvent event(path, event_type);

            if (handler_) {
                try {
                    handler_(event);
                } catch (const std::exception& e) {
                    std::cerr << "[PollingFileWatcher] Handler error: " << e.what()
                              << std::endl;
                }
            }
        }

        old_info = new_info;
    }
}

// =====================================
// 平台特定实现
// =====================================

#ifdef __APPLE__

#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

MacOSFileWatcher::MacOSFileWatcher()
    : kqueue_fd_(-1), running_(false) {}

MacOSFileWatcher::~MacOSFileWatcher() {
    stop();

    if (kqueue_fd_ >= 0) {
        close(kqueue_fd_);
    }

    for (auto& [path, fd] : file_descriptors_) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

bool MacOSFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (file_path.empty()) {
        return false;
    }

    // 如果已经存在，先移除
    remove_file(file_path);

    // 打开文件
    int fd = open(file_path.c_str(), O_EVTONLY);
    if (fd < 0) {
        std::cerr << "[MacOSFileWatcher] Failed to open file: " << file_path
                  << std::endl;
        return false;
    }

    file_descriptors_[file_path] = fd;

    if (running_) {
        // 重新启动以应用更改
        // 简化实现：实际应该动态添加监听
    }

    std::cout << "[MacOSFileWatcher] Added file: " << file_path << std::endl;
    return true;
}

bool MacOSFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_descriptors_.find(file_path);
    if (it != file_descriptors_.end()) {
        if (it->second >= 0) {
            close(it->second);
        }
        file_descriptors_.erase(it);

        std::cout << "[MacOSFileWatcher] Removed file: " << file_path
                  << std::endl;
        return true;
    }
    return false;
}

bool MacOSFileWatcher::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        return false;
    }

    // 创建 kqueue
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ < 0) {
        std::cerr << "[MacOSFileWatcher] Failed to create kqueue" << std::endl;
        return false;
    }

    running_ = true;
    watch_thread_ = std::thread(&MacOSFileWatcher::watch_loop, this);

    std::cout << "[MacOSFileWatcher] Started" << std::endl;
    return true;
}

void MacOSFileWatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }

    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }

    std::cout << "[MacOSFileWatcher] Stopped" << std::endl;
}

void MacOSFileWatcher::set_event_handler(FileEventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

bool MacOSFileWatcher::is_supported() const {
    return true;
}

std::vector<std::string> MacOSFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> files;
    files.reserve(file_descriptors_.size());
    for (const auto& [path, _] : file_descriptors_) {
        files.push_back(path);
    }
    return files;
}

bool MacOSFileWatcher::is_running() const {
    return running_;
}

void MacOSFileWatcher::watch_loop() {
    std::cout << "[MacOSFileWatcher] Watch loop started" << std::endl;

    while (running_) {
        std::vector<struct kevent> changes;
        std::vector<struct kevent> events;

        // 为每个文件添加监听
        {
            std::lock_guard<std::mutex> lock(mutex_);
            changes.reserve(file_descriptors_.size());
            events.resize(file_descriptors_.size());

            for (const auto& [path, fd] : file_descriptors_) {
                struct kevent change;
                EV_SET(&change, fd, EVFILT_VNODE,
                       EV_ADD | EV_ENABLE | EV_ONESHOT,
                       NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB |
                           NOTE_REVOKE,
                       0, nullptr);
                changes.push_back(change);
            }
        }

        if (changes.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int nev = kevent(kqueue_fd_, changes.data(), changes.size(), events.data(),
                         events.size(), nullptr);

        if (nev < 0) {
            if (errno != EINTR) {
                std::cerr << "[MacOSFileWatcher] kevent error: " << strerror(errno)
                          << std::endl;
            }
            continue;
        }

        // 处理事件
        for (int i = 0; i < nev; ++i) {
            int fd = events[i].ident;
            uint32_t fflags = events[i].fflags;

            // 查找对应的文件路径
            std::string file_path;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [path, path_fd] : file_descriptors_) {
                    if (path_fd == fd) {
                        file_path = path;
                        break;
                    }
                }
            }

            if (!file_path.empty()) {
                FileEventType event_type = FileEventType::Modified;

                if (fflags & NOTE_DELETE) {
                    event_type = FileEventType::Deleted;
                } else if (fflags & NOTE_WRITE || fflags & NOTE_EXTEND) {
                    event_type = FileEventType::Modified;
                }

                FileEvent event(file_path, event_type);

                if (handler_) {
                    try {
                        handler_(event);
                    } catch (const std::exception& e) {
                        std::cerr << "[MacOSFileWatcher] Handler error: "
                                  << e.what() << std::endl;
                    }
                }
            }
        }
    }

    std::cout << "[MacOSFileWatcher] Watch loop stopped" << std::endl;
}

#endif  // __APPLE__

#ifdef __linux__

#include <unistd.h>

LinuxFileWatcher::LinuxFileWatcher()
    : inotify_fd_(-1), running_(false) {}

LinuxFileWatcher::~LinuxFileWatcher() {
    stop();

    if (inotify_fd_ >= 0) {
        close(inotify_fd_);
    }
}

bool LinuxFileWatcher::add_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (file_path.empty() || inotify_fd_ < 0) {
        return false;
    }

    // 移除旧监听（如果存在）
    auto it = file_to_wd_.find(file_path);
    if (it != file_to_wd_.end()) {
        inotify_rm_watch(inotify_fd_, it->second);
        watch_descriptors_.erase(it->second);
        file_to_wd_.erase(it);
    }

    // 添加新监听
    int wd = inotify_add_watch(inotify_fd_, file_path.c_str(),
                               IN_MODIFY | IN_DELETE | IN_CREATE | IN_MOVED_FROM |
                                   IN_MOVED_TO);

    if (wd < 0) {
        std::cerr << "[LinuxFileWatcher] Failed to add watch for: "
                  << file_path << std::endl;
        return false;
    }

    watch_descriptors_[wd] = file_path;
    file_to_wd_[file_path] = wd;

    std::cout << "[LinuxFileWatcher] Added file: " << file_path << std::endl;
    return true;
}

bool LinuxFileWatcher::remove_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_to_wd_.find(file_path);
    if (it != file_to_wd_.end()) {
        inotify_rm_watch(inotify_fd_, it->second);
        watch_descriptors_.erase(it->second);
        file_to_wd_.erase(it);

        std::cout << "[LinuxFileWatcher] Removed file: " << file_path
                  << std::endl;
        return true;
    }
    return false;
}

bool LinuxFileWatcher::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        return false;
    }

    inotify_fd_ = inotify_init();
    if (inotify_fd_ < 0) {
        std::cerr << "[LinuxFileWatcher] Failed to initialize inotify"
                  << std::endl;
        return false;
    }

    running_ = true;
    watch_thread_ = std::thread(&LinuxFileWatcher::watch_loop, this);

    std::cout << "[LinuxFileWatcher] Started" << std::endl;
    return true;
}

void LinuxFileWatcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }

    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }

    std::cout << "[LinuxFileWatcher] Stopped" << std::endl;
}

void LinuxFileWatcher::set_event_handler(FileEventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

bool LinuxFileWatcher::is_supported() const {
    return true;
}

std::vector<std::string> LinuxFileWatcher::get_watched_files() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> files;
    files.reserve(file_to_wd_.size());
    for (const auto& [path, _] : file_to_wd_) {
        files.push_back(path);
    }
    return files;
}

bool LinuxFileWatcher::is_running() const {
    return running_;
}

void LinuxFileWatcher::watch_loop() {
    std::cout << "[LinuxFileWatcher] Watch loop started" << std::endl;

    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];

    while (running_) {
        ssize_t length =
            read(inotify_fd_, buffer, BUFFER_SIZE);

        if (length < 0) {
            if (errno != EINTR) {
                std::cerr << "[LinuxFileWatcher] read error: " << strerror(errno)
                          << std::endl;
            }
            continue;
        }

        process_events(buffer, length);
    }

    std::cout << "[LinuxFileWatcher] Watch loop stopped" << std::endl;
}

void LinuxFileWatcher::process_events(const char* buffer, ssize_t length) {
    size_t i = 0;
    while (i < static_cast<size_t>(length)) {
        struct inotify_event* event =
            reinterpret_cast<struct inotify_event*>(const_cast<char*>(buffer + i));

        std::string file_path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = watch_descriptors_.find(event->wd);
            if (it != watch_descriptors_.end()) {
                file_path = it->second;
            }
        }

        if (!file_path.empty() && handler_) {
            FileEventType event_type = FileEventType::Modified;

            if (event->mask & IN_DELETE) {
                event_type = FileEventType::Deleted;
            } else if (event->mask & IN_CREATE) {
                event_type = FileEventType::Created;
            } else if (event->mask & IN_MOVED_FROM) {
                event_type = FileEventType::Moved;
            }

            FileEvent event(file_path, event_type);

            try {
                handler_(event);
            } catch (const std::exception& e) {
                std::cerr << "[LinuxFileWatcher] Handler error: " << e.what()
                          << std::endl;
            }
        }

        i += sizeof(struct inotify_event) + event->len;
    }
}

#endif  // __linux__

}  // namespace shield::fs
