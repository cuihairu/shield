#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "shield/core/service.hpp"

#ifdef SHIELD_ENABLE_PROMETHEUS
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gateway.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/summary.h>
#endif

namespace shield::metrics {

// Metrics collection interface
class MetricsCollector {
public:
    virtual ~MetricsCollector() = default;
    virtual void collect() = 0;
    virtual const std::string &name() const = 0;
};

#ifdef SHIELD_ENABLE_PROMETHEUS

// System metrics collector
class SystemMetricsCollector : public MetricsCollector {
public:
    SystemMetricsCollector(std::shared_ptr<prometheus::Registry> registry);
    void collect() override;
    const std::string &name() const override { return name_; }

private:
    std::string name_ = "system";
    prometheus::Family<prometheus::Gauge> *cpu_usage_family_;
    prometheus::Family<prometheus::Gauge> *memory_usage_family_;
    prometheus::Family<prometheus::Gauge> *memory_total_family_;
    prometheus::Gauge *cpu_usage_gauge_;
    prometheus::Gauge *memory_usage_gauge_;
    prometheus::Gauge *memory_total_gauge_;

    double get_cpu_usage();
    std::pair<size_t, size_t> get_memory_usage();  // used, total
};

// Network metrics collector
class NetworkMetricsCollector : public MetricsCollector {
public:
    NetworkMetricsCollector(std::shared_ptr<prometheus::Registry> registry);
    void collect() override;
    const std::string &name() const override { return name_; }

    // Methods to update metrics from network components
    void increment_connections();
    void decrement_connections();
    void add_bytes_sent(size_t bytes);
    void add_bytes_received(size_t bytes);
    void increment_requests();
    void record_request_duration(double seconds);

    // UDP-specific metrics
    void increment_udp_sessions();
    void decrement_udp_sessions();
    void increment_udp_packets_sent();
    void increment_udp_packets_received();
    void add_udp_bytes_sent(size_t bytes);
    void add_udp_bytes_received(size_t bytes);
    void increment_udp_timeouts();

private:
    std::string name_ = "network";
    prometheus::Family<prometheus::Gauge> *active_connections_family_;
    prometheus::Family<prometheus::Counter> *bytes_sent_family_;
    prometheus::Family<prometheus::Counter> *bytes_received_family_;
    prometheus::Family<prometheus::Counter> *total_requests_family_;
    prometheus::Family<prometheus::Histogram> *request_duration_family_;

    // UDP-specific metrics
    prometheus::Family<prometheus::Gauge> *active_udp_sessions_family_;
    prometheus::Family<prometheus::Counter> *udp_packets_sent_family_;
    prometheus::Family<prometheus::Counter> *udp_packets_received_family_;
    prometheus::Family<prometheus::Counter> *udp_bytes_sent_family_;
    prometheus::Family<prometheus::Counter> *udp_bytes_received_family_;
    prometheus::Family<prometheus::Counter> *udp_timeouts_family_;

    prometheus::Gauge *active_connections_gauge_;
    prometheus::Counter *bytes_sent_counter_;
    prometheus::Counter *bytes_received_counter_;
    prometheus::Counter *total_requests_counter_;
    prometheus::Histogram *request_duration_histogram_;

    // UDP metrics
    prometheus::Gauge *active_udp_sessions_gauge_;
    prometheus::Counter *udp_packets_sent_counter_;
    prometheus::Counter *udp_packets_received_counter_;
    prometheus::Counter *udp_bytes_sent_counter_;
    prometheus::Counter *udp_bytes_received_counter_;
    prometheus::Counter *udp_timeouts_counter_;
};

// Game-specific metrics collector
class GameMetricsCollector : public MetricsCollector {
public:
    GameMetricsCollector(std::shared_ptr<prometheus::Registry> registry);
    void collect() override;
    const std::string &name() const override { return name_; }

    // Methods to update game metrics
    void increment_active_players();
    void decrement_active_players();
    void increment_active_rooms();
    void decrement_active_rooms();
    void increment_messages_processed();
    void increment_actor_created();
    void increment_actor_destroyed();

private:
    std::string name_ = "game";
    prometheus::Family<prometheus::Gauge> *active_players_family_;
    prometheus::Family<prometheus::Gauge> *active_rooms_family_;
    prometheus::Family<prometheus::Counter> *messages_processed_family_;
    prometheus::Family<prometheus::Counter> *actors_created_family_;
    prometheus::Family<prometheus::Counter> *actors_destroyed_family_;

    prometheus::Gauge *active_players_gauge_;
    prometheus::Gauge *active_rooms_gauge_;
    prometheus::Counter *messages_processed_counter_;
    prometheus::Counter *actors_created_counter_;
    prometheus::Counter *actors_destroyed_counter_;
};

#else

// Stub implementations when Prometheus is not available
class SystemMetricsCollector : public MetricsCollector {
public:
    SystemMetricsCollector(std::shared_ptr<void> registry = nullptr) {}
    void collect() override {}
    const std::string &name() const override { return name_; }

private:
    std::string name_ = "system";
};

class NetworkMetricsCollector : public MetricsCollector {
public:
    NetworkMetricsCollector(std::shared_ptr<void> registry = nullptr) {}
    void collect() override {}
    const std::string &name() const override { return name_; }

    void increment_connections() {}
    void decrement_connections() {}
    void add_bytes_sent(size_t bytes) {}
    void add_bytes_received(size_t bytes) {}
    void increment_requests() {}
    void record_request_duration(double seconds) {}

private:
    std::string name_ = "network";
};

class GameMetricsCollector : public MetricsCollector {
public:
    GameMetricsCollector(std::shared_ptr<void> registry = nullptr) {}
    void collect() override {}
    const std::string &name() const override { return name_; }

    void increment_active_players() {}
    void decrement_active_players() {}
    void increment_active_rooms() {}
    void decrement_active_rooms() {}
    void increment_messages_processed() {}
    void increment_actor_created() {}
    void increment_actor_destroyed() {}

private:
    std::string name_ = "game";
};

#endif

// Main Prometheus service
class PrometheusService : public shield::core::ReloadableService {
public:
    PrometheusService();
    ~PrometheusService() override;

    // Get singleton instance
    static PrometheusService &instance();

    // Get metrics collectors
    std::shared_ptr<SystemMetricsCollector> get_system_collector() const {
        return system_collector_;
    }
    std::shared_ptr<NetworkMetricsCollector> get_network_collector() const {
        return network_collector_;
    }
    std::shared_ptr<GameMetricsCollector> get_game_collector() const {
        return game_collector_;
    }

    // Add custom metrics collector
    void add_collector(std::shared_ptr<MetricsCollector> collector);

    // Service interface
    void on_init(core::ApplicationContext& ctx) override;
    void on_start() override;
    void on_stop() override;
    std::string name() const override { return "prometheus"; }
    
    // ReloadableService interface
    void on_config_reloaded() override;

private:
#ifdef SHIELD_ENABLE_PROMETHEUS
    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer> exposer_;
    std::unique_ptr<prometheus::Gateway> gateway_;
#else
    std::shared_ptr<void> registry_;
#endif

    // Built-in collectors
    std::shared_ptr<SystemMetricsCollector> system_collector_;
    std::shared_ptr<NetworkMetricsCollector> network_collector_;
    std::shared_ptr<GameMetricsCollector> game_collector_;

    // Custom collectors
    std::vector<std::shared_ptr<MetricsCollector>> custom_collectors_;

    // Background collection thread
    std::thread collection_thread_;
    std::atomic<bool> running_;
    std::chrono::seconds collection_interval_;

    void collection_loop();
    void collect_all_metrics();

    // Configuration
    std::string listen_address_;
    uint16_t listen_port_;
    std::string job_name_;
    std::string pushgateway_url_;
    bool enable_pushgateway_;
    bool enable_exposer_;
};

}  // namespace shield::metrics