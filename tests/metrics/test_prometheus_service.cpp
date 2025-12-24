// tests/metrics/test_prometheus_service.cpp
#define BOOST_TEST_MODULE PrometheusServiceTests
#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "shield/metrics/prometheus_service.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/metrics/metrics.hpp"

namespace shield::metrics {

// Metrics fixture
class MetricsFixture {
public:
    MetricsFixture() {
        // Reset service state if needed
    }

    ~MetricsFixture() {
        // Cleanup
    }
};

// PrometheusConfig tests
BOOST_AUTO_TEST_SUITE(PrometheusConfigTests)

BOOST_AUTO_TEST_CASE(test_default_config) {
    PrometheusConfig config;

    BOOST_CHECK_EQUAL(config.server.enabled, true);
    BOOST_CHECK_EQUAL(config.server.host, "0.0.0.0");
    BOOST_CHECK_EQUAL(config.server.port, 9090);
    BOOST_CHECK_EQUAL(config.server.path, "/metrics");
    BOOST_CHECK_EQUAL(config.server.max_connections, 100);

    BOOST_CHECK_EQUAL(config.system_metrics.enabled, true);
    BOOST_CHECK_EQUAL(config.system_metrics.collection_interval, 5);
    BOOST_CHECK_EQUAL(config.system_metrics.collect_cpu, true);
    BOOST_CHECK_EQUAL(config.system_metrics.collect_memory, true);
    BOOST_CHECK_EQUAL(config.system_metrics.collect_disk, true);
    BOOST_CHECK_EQUAL(config.system_metrics.collect_network, false);

    BOOST_CHECK_EQUAL(config.app_metrics.enabled, true);
    BOOST_CHECK_EQUAL(config.app_metrics.collect_http_requests, true);
    BOOST_CHECK_EQUAL(config.app_metrics.collect_actor_stats, true);
    BOOST_CHECK_EQUAL(config.app_metrics.collect_gateway_stats, true);
    BOOST_CHECK_EQUAL(config.app_metrics.collect_lua_stats, true);

    BOOST_CHECK_EQUAL(config.export_config.format, "prometheus");
    BOOST_CHECK_EQUAL(config.export_config.include_timestamp, true);
    BOOST_CHECK_EQUAL(config.export_config.include_help_text, true);
    BOOST_CHECK_EQUAL(config.export_config.namespace_prefix, "shield");
}

BOOST_AUTO_TEST_CASE test_custom_config) {
    PrometheusConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 8080;
    config.system_metrics.collection_interval = 10;
    config.export_config.namespace_prefix = "myapp";

    BOOST_CHECK_EQUAL(config.server.host, "127.0.0.1");
    BOOST_CHECK_EQUAL(config.server.port, 8080);
    BOOST_CHECK_EQUAL(config.system_metrics.collection_interval, 10);
    BOOST_CHECK_EQUAL(config.export_config.namespace_prefix, "myapp");
}

BOOST_AUTO_TEST_CASE(test_is_metrics_enabled) {
    PrometheusConfig config;
    BOOST_CHECK(config.is_metrics_enabled());

    config.server.enabled = false;
    BOOST_CHECK(!config.is_metrics_enabled());
}

BOOST_AUTO_TEST_CASE(test_get_metrics_endpoint) {
    PrometheusConfig config;
    std::string endpoint = config.get_metrics_endpoint();
    BOOST_CHECK(endpoint.find("9090") != std::string::npos);
    BOOST_CHECK(endpoint.find("/metrics") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// SystemMetricsCollector tests (stub when Prometheus disabled)
BOOST_AUTO_TEST_SUITE(SystemMetricsCollectorTests)

BOOST_AUTO_TEST_CASE(test_collector_creation) {
    SystemMetricsCollector collector(nullptr);
    BOOST_CHECK_EQUAL(collector.name(), "system");
}

BOOST_AUTO_TEST_CASE(test_collector_collect) {
    SystemMetricsCollector collector(nullptr);
    // Should not throw
    collector.collect();
}

BOOST_AUTO_TEST_SUITE_END()

// NetworkMetricsCollector tests
BOOST_AUTO_TEST_SUITE(NetworkMetricsCollectorTests)

BOOST_AUTO_TEST_CASE(test_network_collector_creation) {
    NetworkMetricsCollector collector(nullptr);
    BOOST_CHECK_EQUAL(collector.name(), "network");
}

BOOST_AUTO_TEST_CASE test_network_collector_increment_connections) {
    NetworkMetricsCollector collector(nullptr);
    // Should not throw
    collector.increment_connections();
    collector.increment_connections();
}

BOOST_AUTO_TEST_CASE test_network_collector_decrement_connections) {
    NetworkMetricsCollector collector(nullptr);
    collector.increment_connections();
    collector.increment_connections();
    collector.decrement_connections();
}

BOOST_AUTO_TEST_CASE test_network_collector_add_bytes_sent) {
    NetworkMetricsCollector collector(nullptr);
    collector.add_bytes_sent(1024);
    collector.add_bytes_sent(2048);
}

BOOST_AUTO_TEST_CASE test_network_collector_add_bytes_received) {
    NetworkMetricsCollector collector(nullptr);
    collector.add_bytes_received(512);
    collector.add_bytes_received(1024);
}

BOOST_AUTO_TEST_CASE test_network_collector_increment_requests) {
    NetworkMetricsCollector collector(nullptr);
    collector.increment_requests();
    collector.increment_requests();
    collector.increment_requests();
}

BOOST_AUTO_TEST_CASE test_network_collector_record_duration) {
    NetworkMetricsCollector collector(nullptr);
    collector.record_request_duration(0.1);
    collector.record_request_duration(0.5);
    collector.record_request_duration(1.0);
}

BOOST_AUTO_TEST_SUITE_END()

// GameMetricsCollector tests
BOOST_AUTO_TEST_SUITE(GameMetricsCollectorTests)

BOOST_AUTO_TEST_CASE(test_game_collector_creation) {
    GameMetricsCollector collector(nullptr);
    BOOST_CHECK_EQUAL(collector.name(), "game");
}

BOOST_AUTO_TEST_CASE(test_game_collector_players) {
    GameMetricsCollector collector(nullptr);
    collector.increment_active_players();
    collector.increment_active_players();
    collector.increment_active_players();
    collector.decrement_active_players();
}

BOOST_AUTO_TEST_CASE(test_game_collector_rooms) {
    GameMetricsCollector collector(nullptr);
    collector.increment_active_rooms();
    collector.increment_active_rooms();
    collector.decrement_active_rooms();
}

BOOST_AUTO_TEST_CASE(test_game_collector_messages) {
    GameMetricsCollector collector(nullptr);
    for (int i = 0; i < 100; ++i) {
        collector.increment_messages_processed();
    }
}

BOOST_AUTO_TEST_CASE(test_game_collector_actors) {
    GameMetricsCollector collector(nullptr);
    collector.increment_actor_created();
    collector.increment_actor_created();
    collector.increment_actor_destroyed();
}

BOOST_AUTO_TEST_SUITE_END()

// Metrics macros tests
BOOST_AUTO_TEST_SUITE(MetricsMacrosTests)

BOOST_AUTO_TEST_CASE(test_metric_inc_connections_macro) {
    // These macros should compile and execute without throwing
    SHIELD_METRIC_INC_CONNECTIONS();
    SHIELD_METRIC_INC_CONNECTIONS();
    SHIELD_METRIC_DEC_CONNECTIONS();
}

BOOST_AUTO_TEST_CASE(test_metric_bytes_macro) {
    SHIELD_METRIC_ADD_BYTES_SENT(1024);
    SHIELD_METRIC_ADD_BYTES_SENT(2048);
    SHIELD_METRIC_ADD_BYTES_RECEIVED(512);
    SHIELD_METRIC_ADD_BYTES_RECEIVED(1024);
}

BOOST_AUTO_TEST_CASE(test_metric_requests_macro) {
    SHIELD_METRIC_INC_REQUESTS();
    SHIELD_METRIC_INC_REQUESTS();
    SHIELD_METRIC_INC_REQUESTS();
}

BOOST_AUTO_TEST_CASE(test_metric_players_macro) {
    SHIELD_METRIC_INC_PLAYERS();
    SHIELD_METRIC_INC_PLAYERS();
    SHIELD_METRIC_DEC_PLAYERS();
}

BOOST_AUTO_TEST_CASE(test_metric_rooms_macro) {
    SHIELD_METRIC_INC_ROOMS();
    SHIELD_METRIC_INC_ROOMS();
    SHIELD_METRIC_DEC_ROOMS();
}

BOOST_AUTO_TEST_CASE(test_metric_messages_macro) {
    for (int i = 0; i < 50; ++i) {
        SHIELD_METRIC_INC_MESSAGES();
    }
}

BOOST_AUTO_TEST_CASE(test_metric_actors_macro) {
    SHIELD_METRIC_INC_ACTORS_CREATED();
    SHIELD_METRIC_INC_ACTORS_CREATED();
    SHIELD_METRIC_INC_ACTORS_DESTROYED();
}

BOOST_AUTO_TEST_SUITE_END()

// RequestTimer tests
BOOST_AUTO_TEST_SUITE(RequestTimerTests)

BOOST_AUTO_TEST_CASE test_request_timer_basic) {
    // Timer should record duration on destruction
    {
        RequestTimer timer;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // After scope, timer destructor is called
}

BOOST_AUTO_TEST_CASE test_request_timer_macro) {
    {
        SHIELD_METRIC_TIME_REQUEST();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Timer records automatically when going out of scope
}

BOOST_AUTO_TEST_CASE(test_multiple_timers) {
    {
        SHIELD_METRIC_TIME_REQUEST();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        SHIELD_METRIC_TIME_REQUEST();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    {
        SHIELD_METRIC_TIME_REQUEST();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

BOOST_AUTO_TEST_SUITE_END()

// MetricsCollector interface tests
BOOST_AUTO_TEST_SUITE(MetricsCollectorInterfaceTests)

class DummyMetricsCollector : public MetricsCollector {
public:
    DummyMetricsCollector(const std::string& name) : name_(name), collect_count_(0) {}

    void collect() override { collect_count_++; }
    const std::string& name() const override { return name_; }
    int collect_count() const { return collect_count_; }

private:
    std::string name_;
    int collect_count_;
};

BOOST_AUTO_TEST_CASE(test_custom_collector) {
    auto collector = std::make_shared<DummyMetricsCollector>("custom");
    BOOST_CHECK_EQUAL(collector->name(), "custom");
    BOOST_CHECK_EQUAL(collector->collect_count(), 0);

    collector->collect();
    BOOST_CHECK_EQUAL(collector->collect_count(), 1);

    collector->collect();
    collector->collect();
    BOOST_CHECK_EQUAL(collector->collect_count(), 3);
}

BOOST_AUTO_TEST_SUITE_END()

// Integration-style tests
BOOST_AUTO_TEST_SUITE(MetricsIntegrationTests)

BOOST_AUTO_TEST_CASE test_complete_network_metrics_flow) {
    NetworkMetricsCollector collector(nullptr);

    // Simulate network activity
    collector.increment_connections();
    collector.add_bytes_sent(1024);
    collector.add_bytes_received(512);
    collector.increment_requests();
    collector.record_request_duration(0.15);

    collector.decrement_connections();

    // Verify collector still works
    collector.collect();
}

BOOST_AUTO_TEST_CASE(test_complete_game_metrics_flow) {
    GameMetricsCollector collector(nullptr);

    // Simulate game activity
    collector.increment_active_players();
    collector.increment_active_players();
    collector.increment_active_rooms();
    collector.increment_messages_processed();
    collector.increment_actor_created();

    collector.collect();

    collector.decrement_active_players();
    collector.increment_actor_destroyed();

    collector.collect();
}

BOOST_AUTO_TEST_CASE(test_concurrent_metrics_updates) {
    NetworkMetricsCollector network_collector(nullptr);
    GameMetricsCollector game_collector(nullptr);

    // Simulate concurrent updates
    std::thread t1([&]() {
        for (int i = 0; i < 100; ++i) {
            network_collector.increment_requests();
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < 100; ++i) {
            game_collector.increment_messages_processed();
        }
    });

    t1.join();
    t2.join();

    // Should not crash
    network_collector.collect();
    game_collector.collect();
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace shield::metrics
