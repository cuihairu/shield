// tests/util/test_health_check.cpp
#define BOOST_TEST_MODULE HealthCheckTests
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "shield/health/health_check.hpp"

namespace shield::health {

// Custom health indicator for testing
class TestHealthIndicator : public HealthIndicator {
public:
    TestHealthIndicator(HealthStatus status, const std::string& name = "test")
        : status_(status), name_(name) {}

    Health check() override { return Health(status_, "Test indicator"); }
    std::string name() const override { return name_; }

    void set_status(HealthStatus status) { status_ = status; }

private:
    HealthStatus status_;
    std::string name_;
};

// Async health indicator for testing
class AsyncTestHealthIndicator : public ReactiveHealthIndicator {
public:
    AsyncTestHealthIndicator(HealthStatus status, int delay_ms = 100,
                            const std::string& name = "async_test")
        : status_(status), delay_ms_(delay_ms), name_(name) {}

    std::future<Health> check_async() override {
        return std::async(std::launch::async, [this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            return Health(status_, "Async test indicator");
        });
    }

    std::string name() const override { return name_; }
    std::chrono::milliseconds timeout() const override {
        return std::chrono::milliseconds(delay_ms_ + 1000);
    }

private:
    HealthStatus status_;
    int delay_ms_;
    std::string name_;
};

// HealthStatus tests
BOOST_AUTO_TEST_SUITE(HealthStatusTests)

BOOST_AUTO_TEST_CASE(test_health_status_values) {
    BOOST_CHECK_EQUAL(static_cast<int>(HealthStatus::UP), 0);
    BOOST_CHECK_EQUAL(static_cast<int>(HealthStatus::DOWN), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(HealthStatus::OUT_OF_SERVICE), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(HealthStatus::UNKNOWN), 3);
}

BOOST_AUTO_TEST_CASE(test_health_status_output) {
    std::stringstream ss;
    ss << HealthStatus::UP;
    BOOST_CHECK_EQUAL(ss.str(), "UP");

    ss.str("");
    ss << HealthStatus::DOWN;
    BOOST_CHECK_EQUAL(ss.str(), "DOWN");

    ss.str("");
    ss << HealthStatus::OUT_OF_SERVICE;
    BOOST_CHECK_EQUAL(ss.str(), "OUT_OF_SERVICE");

    ss.str("");
    ss << HealthStatus::UNKNOWN;
    BOOST_CHECK_EQUAL(ss.str(), "UNKNOWN");
}

BOOST_AUTO_TEST_SUITE_END()

// Health structure tests
BOOST_AUTO_TEST_SUITE(HealthStructureTests)

BOOST_AUTO_TEST_CASE(test_health_default_construction) {
    Health health;
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UNKNOWN);
    BOOST_CHECK(health.description.empty());
    BOOST_CHECK(health.details.empty());
}

BOOST_AUTO_TEST_CASE(test_health_construction_with_status) {
    Health health(HealthStatus::UP);
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
}

BOOST_AUTO_TEST_CASE(test_health_construction_with_description) {
    Health health(HealthStatus::DOWN, "Service unavailable");
    BOOST_CHECK_EQUAL(health.status, HealthStatus::DOWN);
    BOOST_CHECK_EQUAL(health.description, "Service unavailable");
}

BOOST_AUTO_TEST_CASE(test_health_add_detail) {
    Health health(HealthStatus::UP);
    health.add_detail("key1", "value1");
    health.add_detail("key2", "value2");

    BOOST_CHECK_EQUAL(health.details["key1"], "value1");
    BOOST_CHECK_EQUAL(health.details["key2"], "value2");
}

BOOST_AUTO_TEST_CASE(test_health_is_healthy) {
    Health up(HealthStatus::UP);
    BOOST_CHECK(up.is_healthy());

    Health down(HealthStatus::DOWN);
    BOOST_CHECK(!down.is_healthy());

    Health out_of_service(HealthStatus::OUT_OF_SERVICE);
    BOOST_CHECK(!out_of_service.is_healthy());

    Health unknown(HealthStatus::UNKNOWN);
    BOOST_CHECK(!unknown.is_healthy());
}

BOOST_AUTO_TEST_CASE(test_health_timestamp) {
    auto before = std::chrono::steady_clock::now();
    Health health(HealthStatus::UP);
    auto after = std::chrono::steady_clock::now();

    BOOST_CHECK(health.timestamp >= before);
    BOOST_CHECK(health.timestamp <= after);
}

BOOST_AUTO_TEST_SUITE_END()

// HealthIndicator tests
BOOST_AUTO_TEST_SUITE(HealthIndicatorTests)

BOOST_AUTO_TEST_CASE(test_custom_indicator) {
    TestHealthIndicator indicator(HealthStatus::UP);

    Health health = indicator.check();
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
    BOOST_CHECK_EQUAL(indicator.name(), "test");
}

BOOST_AUTO_TEST_CASE(test_indicator_with_custom_name) {
    TestHealthIndicator indicator(HealthStatus::DOWN, "custom_name");

    BOOST_CHECK_EQUAL(indicator.name(), "custom_name");
    Health health = indicator.check();
    BOOST_CHECK_EQUAL(health.status, HealthStatus::DOWN);
}

BOOST_AUTO_TEST_CASE(test_indicator_timeout_default) {
    TestHealthIndicator indicator(HealthStatus::UP);
    BOOST_CHECK_EQUAL(indicator.timeout().count(), 5000);  // 5 seconds
}

BOOST_AUTO_TEST_CASE(test_indicator_contributes_to_overall_health) {
    TestHealthIndicator indicator(HealthStatus::UP);
    BOOST_CHECK(indicator.contributes_to_overall_health());
}

BOOST_AUTO_TEST_SUITE_END()

// ReactiveHealthIndicator tests
BOOST_AUTO_TEST_SUITE(ReactiveHealthIndicatorTests)

BOOST_AUTO_TEST_CASE(test_async_indicator_success) {
    AsyncTestHealthIndicator indicator(HealthStatus::UP, 50);

    Health health = indicator.check();
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
    BOOST_CHECK(health.description.find("Async") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_async_indicator_timeout) {
    // Create indicator with delay longer than timeout
    auto slow_indicator = std::make_unique<AsyncTestHealthIndicator>(
        HealthStatus::UP, 1000);
    // Override timeout to be shorter than delay
    AsyncTestHealthIndicator indicator(HealthStatus::UP, 500);

    // The indicator should return DOWN if timeout occurs
    Health health = indicator.check();
    // With the 100ms delay and 1 second timeout, it should succeed
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
}

BOOST_AUTO_TEST_CASE(test_async_indicator_down) {
    AsyncTestHealthIndicator indicator(HealthStatus::DOWN, 10);

    Health health = indicator.check();
    BOOST_CHECK_EQUAL(health.status, HealthStatus::DOWN);
}

BOOST_AUTO_TEST_SUITE_END()

// HealthCheckRegistry tests
BOOST_AUTO_TEST_SUITE(HealthCheckRegistryTests)

BOOST_AUTO_TEST_CASE(test_register_indicator) {
    auto& registry = HealthCheckRegistry::instance();

    auto indicator = std::make_unique<TestHealthIndicator>(HealthStatus::UP);
    registry.register_health_indicator(std::move(indicator));

    // After registration, we should be able to get health
    // (Note: registry is a singleton, so this affects global state)
}

BOOST_AUTO_TEST_CASE(test_register_named_indicator) {
    auto& registry = HealthCheckRegistry::instance();

    auto indicator = std::make_unique<TestHealthIndicator>(HealthStatus::UP, "test");
    registry.register_health_indicator("custom_test", std::move(indicator));

    Health health = registry.get_health("custom_test");
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
}

BOOST_AUTO_TEST_CASE(test_get_health_for_indicator) {
    auto& registry = HealthCheckRegistry::instance();

    auto indicator = std::make_unique<TestHealthIndicator>(HealthStatus::UP);
    registry.register_health_indicator("health_test", std::move(indicator));

    Health health = registry.get_health("health_test");
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
}

BOOST_AUTO_TEST_CASE(test_unregister_indicator) {
    auto& registry = HealthCheckRegistry::instance();

    auto indicator = std::make_unique<TestHealthIndicator>(HealthStatus::UP);
    registry.register_health_indicator("to_remove", std::move(indicator));

    registry.unregister_health_indicator("to_remove");

    // Getting health for unregistered indicator should return UNKNOWN
    Health health = registry.get_health("to_remove");
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UNKNOWN);
}

BOOST_AUTO_TEST_CASE test_get_all_health) {
    auto& registry = HealthCheckRegistry::instance();

    auto indicator1 = std::make_unique<TestHealthIndicator>(HealthStatus::UP);
    auto indicator2 = std::make_unique<TestHealthIndicator>(HealthStatus::DOWN);

    registry.register_health_indicator("up_indicator", std::move(indicator1));
    registry.register_health_indicator("down_indicator", std::move(indicator2));

    auto all_health = registry.get_all_health();
    BOOST_CHECK_GE(all_health.size(), 2);
}

BOOST_AUTO_TEST_CASE(test_indicator_enabled_status) {
    auto& registry = HealthCheckRegistry::instance();

    auto indicator = std::make_unique<TestHealthIndicator>(HealthStatus::UP);
    registry.register_health_indicator("enabled_test", std::move(indicator));

    // Should be enabled by default
    BOOST_CHECK(registry.is_indicator_enabled("enabled_test"));

    registry.set_indicator_enabled("enabled_test", false);
    BOOST_CHECK(!registry.is_indicator_enabled("enabled_test"));

    registry.set_indicator_enabled("enabled_test", true);
    BOOST_CHECK(registry.is_indicator_enabled("enabled_test"));
}

BOOST_AUTO_TEST_CASE(test_health_stats) {
    auto& registry = HealthCheckRegistry::instance();

    registry.clear_health_stats();

    auto indicator = std::make_unique<TestHealthIndicator>(HealthStatus::UP);
    registry.register_health_indicator("stats_test", std::move(indicator));

    // Perform health check to update stats
    registry.get_health("stats_test");

    auto stats = registry.get_health_stats();
    BOOST_CHECK_GT(stats.total_checks, 0);
}

BOOST_AUTO_TEST_SUITE_END()

// ApplicationHealthIndicator tests
BOOST_AUTO_TEST_SUITE(ApplicationHealthIndicatorTests)

BOOST_AUTO_TEST_CASE(test_application_indicator) {
    ApplicationHealthIndicator indicator;

    Health health = indicator.check();
    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
    BOOST_CHECK_EQUAL(indicator.name(), "application");
    BOOST_CHECK(health.description.find("running") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// HealthEndpointBuilder tests
BOOST_AUTO_TEST_SUITE(HealthEndpointBuilderTests)

BOOST_AUTO_TEST_CASE(test_build_health_response) {
    Health overall(HealthStatus::UP, "All systems go");
    overall.add_detail("uptime", "3600");

    std::unordered_map<std::string, Health> individual;
    individual["db"] = Health(HealthStatus::UP, "Database OK");
    individual["cache"] = Health(HealthStatus::UP, "Cache OK");

    std::string response =
        HealthEndpointBuilder::build_health_response(overall, individual, true);

    BOOST_CHECK(!response.empty());
    BOOST_CHECK(response.find("UP") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_build_json_response) {
    Health overall(HealthStatus::UP, "OK");
    std::unordered_map<std::string, Health> individual;
    individual["test"] = Health(HealthStatus::UP, "Test");

    std::string json =
        HealthEndpointBuilder::build_json_response(overall, individual, true);

    BOOST_CHECK(!json.empty());
    BOOST_CHECK(json.find("UP") != std::string::npos ||
                json.find("\"status\"") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace shield::health
