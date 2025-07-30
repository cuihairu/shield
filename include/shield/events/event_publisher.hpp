#pragma once

#include <algorithm>
#include <thread>

#include "shield/events/event_system.hpp"
#include "shield/log/logger.hpp"

namespace shield::events {

// 默认事件发布器实现
class DefaultEventPublisher : public EventPublisher {
private:
    struct ListenerInfo {
        std::shared_ptr<void> listener;
        bool async;
        int order;

        bool operator<(const ListenerInfo& other) const {
            return order < other.order;  // 按order排序
        }
    };

    std::unordered_map<std::type_index, std::vector<ListenerInfo>> listeners_;
    std::mutex listeners_mutex_;

    // 异步事件处理线程池
    std::vector<std::thread> worker_threads_;
    std::queue<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::atomic<bool> shutdown_{false};
    static constexpr size_t THREAD_POOL_SIZE = 4;

public:
    DefaultEventPublisher() {
        // 启动异步处理线程池
        for (size_t i = 0; i < THREAD_POOL_SIZE; ++i) {
            worker_threads_.emplace_back([this] { worker_thread_loop(); });
        }
    }

    ~DefaultEventPublisher() { shutdown(); }

    void publish_event(std::shared_ptr<Event> event) override {
        if (!event) return;

        std::type_index event_type(typeid(*event));
        std::lock_guard<std::mutex> lock(listeners_mutex_);

        auto it = listeners_.find(event_type);
        if (it == listeners_.end()) {
            SHIELD_LOG_DEBUG << "No listeners for event: "
                             << event->get_event_type();
            return;
        }

        SHIELD_LOG_DEBUG << "Publishing event: " << event->get_event_type()
                         << " to " << it->second.size() << " listeners";

        // 按order排序执行
        auto sorted_listeners = it->second;
        std::sort(sorted_listeners.begin(), sorted_listeners.end());

        for (const auto& listener_info : sorted_listeners) {
            if (listener_info.async) {
                // 异步执行
                enqueue_async_task([event, listener_info]() {
                    invoke_listener(event, listener_info);
                });
            } else {
                // 同步执行
                invoke_listener(event, listener_info);
            }
        }
    }

protected:
    void register_listener(std::type_index event_type,
                           std::shared_ptr<void> listener) override {
        std::lock_guard<std::mutex> lock(listeners_mutex_);

        // 获取监听器的元信息
        bool async = false;
        int order = 0;

        // 这里使用类型擦除的方式获取监听器信息
        // 实际实现中可能需要更复杂的类型推导
        if (auto base_listener =
                std::static_pointer_cast<EventListener<Event>>(listener)) {
            async = base_listener->supports_async();
            order = base_listener->get_order();
        }

        listeners_[event_type].push_back({listener, async, order});

        SHIELD_LOG_DEBUG << "Registered listener for event type: "
                         << event_type.name() << " (async: " << async
                         << ", order: " << order << ")";
    }

private:
    static void invoke_listener(std::shared_ptr<Event> event,
                                const ListenerInfo& listener_info) {
        try {
            // 这里需要类型安全的调用，实际实现可能需要更复杂的类型系统
            // 简化版本：直接调用
            SHIELD_LOG_DEBUG << "Invoking listener for event: "
                             << event->get_event_type();

            // 实际的监听器调用需要基于具体的事件类型进行类型转换
            // 这里展示概念，实际实现需要使用模板特化或其他类型安全机制

        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception in event listener: " << e.what();
        }
    }

    void enqueue_async_task(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (shutdown_) return;
            task_queue_.push(std::move(task));
        }
        queue_condition_.notify_one();
    }

    void worker_thread_loop() {
        while (!shutdown_) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_condition_.wait(
                    lock, [this] { return shutdown_ || !task_queue_.empty(); });

                if (shutdown_ && task_queue_.empty()) break;
                if (task_queue_.empty()) continue;

                task = std::move(task_queue_.front());
                task_queue_.pop();
            }

            try {
                task();
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "Exception in async event handler: "
                                 << e.what();
            }
        }
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            shutdown_ = true;
        }
        queue_condition_.notify_all();

        for (auto& thread : worker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
};

// 全局事件发布器单例
class GlobalEventPublisher {
public:
    static EventPublisher& instance() {
        static DefaultEventPublisher instance;
        return instance;
    }

    // 便捷方法
    template <typename EventType, typename... Args>
    static void emit(Args&&... args) {
        instance().emit_event<EventType>(std::forward<Args>(args)...);
    }

    template <typename EventType>
    static void listen(std::function<void(const EventType&)> handler,
                       bool async = false, int order = 0) {
        instance().on<EventType>(std::move(handler), async, order);
    }
};

}  // namespace shield::events