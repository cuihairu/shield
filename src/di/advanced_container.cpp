#include "shield/di/advanced_container.hpp"

#include "shield/log/logger.hpp"

namespace shield::di {

ApplicationEventPublisher::ApplicationEventPublisher() : running_(false) {}

ApplicationEventPublisher::~ApplicationEventPublisher() { stop(); }

void ApplicationEventPublisher::publish_event_internal(
    const ApplicationEvent& event, std::type_index event_type) {
    std::lock_guard<std::mutex> lock(listeners_mutex_);

    auto it = listeners_.find(event_type);
    if (it == listeners_.end()) {
        return;  // No listeners for this event type
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_published++;
    }

    // Process listeners in priority order
    for (const auto& listener_info : it->second) {
        try {
            if (listener_info.async && running_) {
                // Queue for async processing
                std::lock_guard<std::mutex> queue_lock(async_queue_mutex_);
                if (async_queue_.size() < async_queue_limit_) {
                    // Clone the event for async processing
                    async_queue_.emplace(
                        std::make_unique<ApplicationEvent>(event), event_type);
                    async_queue_cv_.notify_one();
                }
            } else {
                // Synchronous processing
                listener_info.handler(event);

                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.total_listeners_called++;
            }
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception in event listener '"
                             << listener_info.name << "': " << e.what();

            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.failed_deliveries++;
        }
    }
}

void ApplicationEventPublisher::remove_listener(const std::string& name) {
    std::lock_guard<std::mutex> lock(listeners_mutex_);

    for (auto& [event_type, listeners] : listeners_) {
        listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                       [&name](const EventListenerInfo& info) {
                                           return info.name == name;
                                       }),
                        listeners.end());
    }
}

EventStats ApplicationEventPublisher::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void ApplicationEventPublisher::clear_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = EventStats{};
}

void ApplicationEventPublisher::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    async_processor_thread_ =
        std::thread(&ApplicationEventPublisher::process_async_events, this);
    SHIELD_LOG_INFO << "ApplicationEventPublisher started";
}

void ApplicationEventPublisher::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    async_queue_cv_.notify_all();

    if (async_processor_thread_.joinable()) {
        async_processor_thread_.join();
    }

    SHIELD_LOG_INFO << "ApplicationEventPublisher stopped";
}

void ApplicationEventPublisher::process_async_events() {
    while (running_) {
        std::unique_lock<std::mutex> lock(async_queue_mutex_);
        async_queue_cv_.wait(
            lock, [this] { return !async_queue_.empty() || !running_; });

        while (!async_queue_.empty() && running_) {
            auto [event, event_type] = std::move(async_queue_.front());
            async_queue_.pop();
            lock.unlock();

            // Process async listeners
            {
                std::lock_guard<std::mutex> listeners_lock(listeners_mutex_);
                auto it = listeners_.find(event_type);
                if (it != listeners_.end()) {
                    for (const auto& listener_info : it->second) {
                        if (listener_info.async) {
                            try {
                                listener_info.handler(*event);

                                std::lock_guard<std::mutex> stats_lock(
                                    stats_mutex_);
                                stats_.total_listeners_called++;
                            } catch (const std::exception& e) {
                                SHIELD_LOG_ERROR
                                    << "Exception in async event listener '"
                                    << listener_info.name << "': " << e.what();

                                std::lock_guard<std::mutex> stats_lock(
                                    stats_mutex_);
                                stats_.failed_deliveries++;
                            }
                        }
                    }
                }
            }

            lock.lock();
        }
    }
}

void ApplicationEventPublisher::update_stats(
    std::chrono::milliseconds processing_time) {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    // Simple moving average for processing time
    if (stats_.total_published > 0) {
        auto total_time_ms =
            stats_.avg_processing_time.count() * (stats_.total_published - 1) +
            processing_time.count();
        stats_.avg_processing_time =
            std::chrono::milliseconds(total_time_ms / stats_.total_published);
    } else {
        stats_.avg_processing_time = processing_time;
    }
}

// Global event publisher instance
ApplicationEventPublisher& get_global_event_publisher() {
    static ApplicationEventPublisher instance;
    return instance;
}

}  // namespace shield::di