// [CORE]
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_set>

#include "shield/events/event_publisher.hpp"
#include "shield/events/event_system.hpp"
#include "shield/fs/file_watcher.hpp"

namespace shield::config {

// 配置写入意图事件
namespace events {
class ConfigWriteIntentEvent : public shield::events::Event {
public:
    ConfigWriteIntentEvent(const std::string& file_path,
                           const std::string& source)
        : Event(source), file_path_(file_path) {}

    std::string get_event_type() const override {
        return "ConfigWriteIntentEvent";
    }
    const std::string& get_file_path() const { return file_path_; }

private:
    std::string file_path_;
};

class ConfigWriteCompletedEvent : public shield::events::Event {
public:
    ConfigWriteCompletedEvent(const std::string& file_path, bool success,
                              const std::string& source)
        : Event(source), file_path_(file_path), success_(success) {}

    std::string get_event_type() const override {
        return "ConfigWriteCompletedEvent";
    }
    const std::string& get_file_path() const { return file_path_; }
    bool is_success() const { return success_; }

private:
    std::string file_path_;
    bool success_;
};
}  // namespace events

// 智能文件监控器 - 能够区分内部写入和外部变化
class SmartFileWatcher {
private:
    struct WriteSession {
        std::string file_path;
        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point expected_completion;
        std::string source;
        std::atomic<bool> completed{false};
    };

    fs::FileWatcher file_watcher_;
    std::unordered_map<std::string, std::unique_ptr<WriteSession>>
        active_writes_;
    std::mutex writes_mutex_;
    std::atomic<uint64_t> session_id_counter_{0};

    // 配置参数
    std::chrono::milliseconds write_timeout_{5000};  // 写入超时时间
    std::chrono::milliseconds settle_delay_{100};    // 文件稳定等待时间
    std::chrono::milliseconds debounce_interval_{50};  // 防抖间隔

public:
    SmartFileWatcher() { setup_event_listeners(); }

    // 启动文件监控
    void start() {
        file_watcher_.start();
        SHIELD_LOG_INFO << "SmartFileWatcher started";
    }

    // 停止文件监控
    void stop() {
        file_watcher_.stop();
        SHIELD_LOG_INFO << "SmartFileWatcher stopped";
    }

    // 添加监控文件
    void watch_file(const std::string& file_path) {
        file_watcher_.add_file(file_path);

        // 注册文件变化处理器
        file_watcher_.add_handler(
            [this](const fs::FileEvent& event) { handle_file_event(event); });

        SHIELD_LOG_INFO << "Now watching file: " << file_path;
    }

private:
    void setup_event_listeners() {
        // 监听配置写入意图事件
        shield::events::GlobalEventPublisher::listen<
            events::ConfigWriteIntentEvent>(
            [this](const events::ConfigWriteIntentEvent& event) {
                register_internal_write(
                    event.get_file_path(),
                    std::any_cast<std::string>(event.get_source()));
            });

        // 监听配置写入完成事件
        shield::events::GlobalEventPublisher::listen<
            events::ConfigWriteCompletedEvent>(
            [this](const events::ConfigWriteCompletedEvent& event) {
                unregister_internal_write(event.get_file_path(),
                                          event.is_success());
            });
    }

    void register_internal_write(const std::string& file_path,
                                 const std::string& source) {
        std::lock_guard<std::mutex> lock(writes_mutex_);

        auto session = std::make_unique<WriteSession>();
        session->file_path = file_path;
        session->start_time = std::chrono::steady_clock::now();
        session->expected_completion = session->start_time + write_timeout_;
        session->source = source;

        active_writes_[file_path] = std::move(session);

        SHIELD_LOG_DEBUG << "Registered internal write for: " << file_path
                         << " from: " << source;
    }

    void unregister_internal_write(const std::string& file_path, bool success) {
        std::lock_guard<std::mutex> lock(writes_mutex_);

        auto it = active_writes_.find(file_path);
        if (it != active_writes_.end()) {
            it->second->completed = true;

            // 延迟移除，给文件系统事件一些时间到达
            std::thread([this, file_path, success]() {
                std::this_thread::sleep_for(settle_delay_);

                std::lock_guard<std::mutex> lock(writes_mutex_);
                active_writes_.erase(file_path);

                SHIELD_LOG_DEBUG
                    << "Unregistered internal write for: " << file_path
                    << " (success: " << success << ")";
            }).detach();
        }
    }

    void handle_file_event(const fs::FileEvent& event) {
        if (event.event_type != fs::FileEventType::Modified) {
            return;  // 只关心修改事件
        }

        // 检查是否是内部写入
        if (is_internal_write(event.file_path)) {
            SHIELD_LOG_DEBUG << "Ignoring internal write for: "
                             << event.file_path;
            return;
        }

        // 防抖处理 - 避免连续的文件系统事件
        if (should_debounce(event.file_path)) {
            SHIELD_LOG_DEBUG << "Debouncing file event for: "
                             << event.file_path;
            return;
        }

        // 处理外部配置变化
        handle_external_config_change(event.file_path);
    }

    bool is_internal_write(const std::string& file_path) {
        std::lock_guard<std::mutex> lock(writes_mutex_);

        auto it = active_writes_.find(file_path);
        if (it == active_writes_.end()) {
            return false;  // 没有活跃的内部写入
        }

        auto& session = it->second;
        auto now = std::chrono::steady_clock::now();

        // 检查写入是否已完成
        if (session->completed) {
            return true;  // 刚完成的写入，忽略后续事件
        }

        // 检查是否超时
        if (now > session->expected_completion) {
            SHIELD_LOG_WARN << "Internal write timeout for: " << file_path;
            active_writes_.erase(it);
            return false;  // 超时，视为外部变化
        }

        return true;  // 活跃的内部写入
    }

    bool should_debounce(const std::string& file_path) {
        static std::unordered_map<std::string,
                                  std::chrono::steady_clock::time_point>
            last_events;
        static std::mutex debounce_mutex;

        std::lock_guard<std::mutex> lock(debounce_mutex);

        auto now = std::chrono::steady_clock::now();
        auto it = last_events.find(file_path);

        if (it != last_events.end()) {
            auto time_since_last = now - it->second;
            if (time_since_last < debounce_interval_) {
                it->second = now;  // 更新时间戳
                return true;       // 需要防抖
            }
        }

        last_events[file_path] = now;
        return false;  // 不需要防抖
    }

    void handle_external_config_change(const std::string& file_path) {
        SHIELD_LOG_INFO << "🔄 External config change detected: " << file_path;

        // 发布配置文件变化事件
        shield::events::GlobalEventPublisher::emit<
            shield::events::config::ConfigRefreshEvent>(
            std::string("ExternalFileChange:") + file_path);
    }
};

}  // namespace shield::config