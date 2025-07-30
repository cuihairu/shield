#include "shield/config/file_watcher.hpp"

#include <mutex>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

namespace shield::config {

// ============================================================================
// FileWatcher Implementation
// ============================================================================

FileWatcher::FileWatcher(std::chrono::milliseconds poll_interval)
    : fs_watcher_(std::make_unique<shield::fs::FileWatcher>(poll_interval)) {
    // 注册文件变更事件处理器
    handler_id_ = fs_watcher_->add_handler(
        [this](const shield::fs::FileEvent& event) { on_file_changed(event); });
}

FileWatcher::FileWatcher(const std::string& file_path,
                         std::chrono::milliseconds interval,
                         ConfigFormat format)
    : FileWatcher(interval) {
    // 兼容性构造函数，自动添加单个文件
    add_config_file(file_path, format);
}

FileWatcher::~FileWatcher() {
    if (fs_watcher_) {
        fs_watcher_->remove_handler(handler_id_);
        fs_watcher_->stop();
    }
}

bool FileWatcher::add_config_file(const std::string& file_path,
                                  ConfigFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (file_formats_.find(file_path) != file_formats_.end()) {
        SHIELD_LOG_DEBUG << "Config file already being watched: " << file_path;
        return true;
    }

    if (fs_watcher_->add_file(file_path)) {
        file_formats_[file_path] = format;
        SHIELD_LOG_INFO << "Added config file to watcher: " << file_path;
        return true;
    }

    return false;
}

bool FileWatcher::remove_config_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_formats_.find(file_path);
    if (it == file_formats_.end()) {
        return true;  // Not watching this file
    }

    if (fs_watcher_->remove_file(file_path)) {
        file_formats_.erase(it);
        SHIELD_LOG_INFO << "Removed config file from watcher: " << file_path;
        return true;
    }

    return false;
}

std::vector<std::string> FileWatcher::get_watched_files() const {
    return fs_watcher_->get_watched_files();
}

void FileWatcher::start() {
    fs_watcher_->start();

    std::lock_guard<std::mutex> lock(mutex_);
    SHIELD_LOG_INFO << "Started config file watcher for "
                    << file_formats_.size() << " files";
}

void FileWatcher::stop() {
    fs_watcher_->stop();
    SHIELD_LOG_INFO << "Stopped config file watcher";
}

bool FileWatcher::is_running() const { return fs_watcher_->is_running(); }

bool FileWatcher::is_native_supported() const {
    return fs_watcher_->is_native_supported();
}

void FileWatcher::on_file_changed(const shield::fs::FileEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = file_formats_.find(event.file_path);
    if (it == file_formats_.end()) {
        return;  // Not watching this file
    }

    ConfigFormat format = it->second;

    switch (event.event_type) {
        case shield::fs::FileEventType::Modified:
        case shield::fs::FileEventType::Created:
            SHIELD_LOG_INFO << "Config file changed: " << event.file_path
                            << ", reloading...";
            reload_config_file(event.file_path, format);
            break;

        case shield::fs::FileEventType::Deleted:
            SHIELD_LOG_WARN << "Config file deleted: " << event.file_path;
            break;

        case shield::fs::FileEventType::Moved:
            SHIELD_LOG_INFO << "Config file moved: " << event.old_path << " -> "
                            << event.file_path;
            reload_config_file(event.file_path, format);
            break;
    }
}

void FileWatcher::reload_config_file(const std::string& file_path,
                                     ConfigFormat format) {
    try {
        ConfigManager::instance().reload_config(file_path, format);
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to reload config file " << file_path << ": "
                         << e.what();
    }
}

}  // namespace shield::config