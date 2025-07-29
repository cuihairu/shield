#pragma once

#include "shield/metrics/prometheus_component.hpp"
#include <chrono>
#include <memory>

namespace shield::metrics {

// Convenience macros for metrics collection
#define SHIELD_METRICS() PrometheusComponent::instance()

#define SHIELD_METRIC_INC_CONNECTIONS() \
    do { \
        auto collector = SHIELD_METRICS().get_network_collector(); \
        if (collector) collector->increment_connections(); \
    } while(0)

#define SHIELD_METRIC_DEC_CONNECTIONS() \
    do { \
        auto collector = SHIELD_METRICS().get_network_collector(); \
        if (collector) collector->decrement_connections(); \
    } while(0)

#define SHIELD_METRIC_ADD_BYTES_SENT(bytes) \
    do { \
        auto collector = SHIELD_METRICS().get_network_collector(); \
        if (collector) collector->add_bytes_sent(bytes); \
    } while(0)

#define SHIELD_METRIC_ADD_BYTES_RECEIVED(bytes) \
    do { \
        auto collector = SHIELD_METRICS().get_network_collector(); \
        if (collector) collector->add_bytes_received(bytes); \
    } while(0)

#define SHIELD_METRIC_INC_REQUESTS() \
    do { \
        auto collector = SHIELD_METRICS().get_network_collector(); \
        if (collector) collector->increment_requests(); \
    } while(0)

#define SHIELD_METRIC_INC_PLAYERS() \
    do { \
        auto collector = SHIELD_METRICS().get_game_collector(); \
        if (collector) collector->increment_active_players(); \
    } while(0)

#define SHIELD_METRIC_DEC_PLAYERS() \
    do { \
        auto collector = SHIELD_METRICS().get_game_collector(); \
        if (collector) collector->decrement_active_players(); \
    } while(0)

#define SHIELD_METRIC_INC_ROOMS() \
    do { \
        auto collector = SHIELD_METRICS().get_game_collector(); \
        if (collector) collector->increment_active_rooms(); \
    } while(0)

#define SHIELD_METRIC_DEC_ROOMS() \
    do { \
        auto collector = SHIELD_METRICS().get_game_collector(); \
        if (collector) collector->decrement_active_rooms(); \
    } while(0)

#define SHIELD_METRIC_INC_MESSAGES() \
    do { \
        auto collector = SHIELD_METRICS().get_game_collector(); \
        if (collector) collector->increment_messages_processed(); \
    } while(0)

#define SHIELD_METRIC_INC_ACTORS_CREATED() \
    do { \
        auto collector = SHIELD_METRICS().get_game_collector(); \
        if (collector) collector->increment_actor_created(); \
    } while(0)

#define SHIELD_METRIC_INC_ACTORS_DESTROYED() \
    do { \
        auto collector = SHIELD_METRICS().get_game_collector(); \
        if (collector) collector->increment_actor_destroyed(); \
    } while(0)

// RAII timer for measuring request durations
class RequestTimer {
public:
    RequestTimer() : start_time_(std::chrono::steady_clock::now()) {}
    
    ~RequestTimer() {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_);
        double seconds = duration.count() / 1000000.0;
        
        auto collector = SHIELD_METRICS().get_network_collector();
        if (collector) {
            collector->record_request_duration(seconds);
        }
    }

private:
    std::chrono::steady_clock::time_point start_time_;
};

#define SHIELD_METRIC_TIME_REQUEST() shield::metrics::RequestTimer _timer

} // namespace shield::metrics