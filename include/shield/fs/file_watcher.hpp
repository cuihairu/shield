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

// 文件事件类型
enum class FileEventType {
    Modified,  // 文件被修改
    Created,   // 文件被创建
    Deleted,   // 文件被删除
    Moved      // 文件被移动/重命名
};

// 文件事件
struct FileEvent {
    std::string file_path;                            // 事件发生的文件路径
    FileEventType event_type;                         // 事件类型
    std::string old_path;                             // 移动事件的原路径
    std::chrono::system_clock::time_point timestamp;  // 事件时间戳

    FileEvent(const std::string& path, FileEventType type,
              const std::string& old_p = "")
        : file_path(path),
          event_type(type),
          old_path(old_p),
          timestamp(std::chrono::system_clock::now()) {}
};

// 文件事件处理器

using FileEventHandler = std::function<void(const FileEvent&)>;

// 文件监控接口
class IFileWatcher {
public:
    virtual ~IFileWatcher() = default;

    // 添加文件监控
    virtual bool add_file(const std::string& file_path) = 0;

    // 移除文件监控
    virtual bool remove_file(const std::string& file_path) = 0;

    // 开始监控
    virtual bool start() = 0;

    // 停止监控
    virtual void stop() = 0;

    // 设置事件处理器
    virtual void set_event_handler(FileEventHandler handler) = 0;

    // 检查是否支持当前平台
    virtual bool is_supported() const = 0;

    // 获取监控的文件列表
    virtual std::vector<std::string> get_watched_files() const = 0;

    // 检查是否正在运行
    virtual bool is_running() const = 0;
};

// 文件监控器工厂
class FileWatcherFactory {
public:
    // 创建最佳的文件监控器（优先原生API）
    static std::unique_ptr<IFileWatcher> create_best_watcher(
        std::chrono::milliseconds poll_interval =
            std::chrono::milliseconds(1000));

    // 创建轮询监控器
    static std::unique_ptr<IFileWatcher> create_polling_watcher(
        std::chrono::milliseconds poll_interval =
            std::chrono::milliseconds(1000));

    // 创建原生监控器（如果支持）
    static std::unique_ptr<IFileWatcher> create_native_watcher();

    // 检查原生监控器是否可用
    static bool is_native_supported();
};

// 事件分发器 - 支持多个处理器
class FileEventDispatcher {
public:
    using HandlerId = size_t;

    // 添加事件处理器
    HandlerId add_handler(FileEventHandler handler);

    // 移除事件处理器
    void remove_handler(HandlerId id);

    // 分发事件到所有处理器
    void dispatch(const FileEvent& event);

    // 清空所有处理器
    void clear();

    // 获取处理器数量
    size_t handler_count() const;

private:
    std::unordered_map<HandlerId, FileEventHandler> handlers_;
    HandlerId next_id_ = 1;
    mutable std::mutex mutex_;
};

// 文件监控管理器 - 全局单例
class FileWatchManager {
public:
    static FileWatchManager& instance();

    // 创建新的监控器
    std::shared_ptr<IFileWatcher> create_watcher(
        const std::string& name, std::chrono::milliseconds poll_interval =
                                     std::chrono::milliseconds(1000));

    // 获取已存在的监控器
    std::shared_ptr<IFileWatcher> get_watcher(const std::string& name);

    // 移除监控器
    void remove_watcher(const std::string& name);

    // 获取所有监控器名称
    std::vector<std::string> get_watcher_names() const;

    // 停止所有监控器
    void stop_all();

    // 启动所有监控器
    void start_all();

private:
    FileWatchManager() = default;
    std::unordered_map<std::string, std::shared_ptr<IFileWatcher>> watchers_;
    mutable std::mutex mutex_;
};

// 便利的文件监控器包装类
class FileWatcher {
public:
    explicit FileWatcher(std::chrono::milliseconds poll_interval =
                             std::chrono::milliseconds(1000));
    explicit FileWatcher(std::shared_ptr<IFileWatcher> impl);
    ~FileWatcher();

    // 基本操作
    bool add_file(const std::string& file_path);
    bool remove_file(const std::string& file_path);
    void start();
    void stop();

    // 事件处理
    FileEventDispatcher::HandlerId add_handler(FileEventHandler handler);
    void remove_handler(FileEventDispatcher::HandlerId id);

    // 状态查询
    bool is_running() const;
    bool is_native_supported() const;
    std::vector<std::string> get_watched_files() const;

    // 获取底层实现
    std::shared_ptr<IFileWatcher> get_impl() const { return impl_; }

private:
    std::shared_ptr<IFileWatcher> impl_;
    std::unique_ptr<FileEventDispatcher> dispatcher_;
};

}  // namespace shield::fs