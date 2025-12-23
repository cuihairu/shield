// shield/tests/health/test_health_check.cpp
#define BOOST_TEST_MODULE health_check_tests
#include <boost/test/unit_test.hpp>
#include <thread>
#include <chrono>

#include "shield/health/health_check.hpp"

using namespace shield::health;

BOOST_AUTO_TEST_SUITE(health_check_suite)

// =====================================
// DiskSpaceHealthIndicator 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_disk_space_indicator_construction) {
    DiskSpaceHealthIndicator indicator("/tmp", 1024 * 1024);  // 1MB threshold

    BOOST_CHECK_EQUAL(indicator.name(), "diskSpace");
    BOOST_CHECK(indicator.timeout() == std::chrono::milliseconds(5000));
}

BOOST_AUTO_TEST_CASE(test_disk_space_indicator_check) {
    DiskSpaceHealthIndicator indicator("/tmp", 1);  // 1 byte threshold

    Health health = indicator.check();

    BOOST_CHECK(health.status == HealthStatus::UP ||
                health.status == HealthStatus::DOWN);

    BOOST_CHECK(!health.description.empty());
    BOOST_CHECK(health.details.find("path") != health.details.end());
    BOOST_CHECK(health.details.at("path") == "/tmp");
}

BOOST_AUTO_TEST_CASE(test_disk_space_indicator_nonexistent_path) {
    DiskSpaceHealthIndicator indicator("/nonexistent/path/that/does/not/exist", 1024);

    Health health = indicator.check();

    // 应该返回 DOWN 或 UNKNOWN
    BOOST_CHECK(health.status == HealthStatus::DOWN ||
                health.status == HealthStatus::UNKNOWN);
}

// =====================================
// ApplicationHealthIndicator 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_application_health_indicator) {
    ApplicationHealthIndicator indicator;

    Health health = indicator.check();

    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
    BOOST_CHECK_EQUAL(indicator.name(), "application");
    BOOST_CHECK(health.is_healthy());
    BOOST_CHECK(health.description.find("running") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_application_health_indicator_details) {
    ApplicationHealthIndicator indicator;

    Health health = indicator.check();

    BOOST_CHECK(health.details.find("uptime") != health.details.end());
    BOOST_CHECK(health.details.find("version") != health.details.end());
}

// =====================================
// Health 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_health_construction) {
    Health health(HealthStatus::UP, "Test description");

    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
    BOOST_CHECK_EQUAL(health.description, "Test description");
    BOOST_CHECK(health.is_healthy());
}

BOOST_AUTO_TEST_CASE(test_health_add_detail) {
    Health health(HealthStatus::UP, "Test");

    health.add_detail("key1", "value1")
          .add_detail("key2", "value2");

    BOOST_CHECK_EQUAL(health.details["key1"], "value1");
    BOOST_CHECK_EQUAL(health.details["key2"], "value2");
}

BOOST_AUTO_TEST_CASE(test_health_default_status) {
    Health health;  // 默认构造

    BOOST_CHECK_EQUAL(health.status, HealthStatus::UNKNOWN);
    BOOST_CHECK(!health.is_healthy());
}

// =====================================
// HealthCheckRegistry 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_health_registry_singleton) {
    auto& registry1 = HealthCheckRegistry::instance();
    auto& registry2 = HealthCheckRegistry::instance();

    BOOST_CHECK(&registry1 == &registry2);
}

BOOST_AUTO_TEST_CASE(test_health_registry_register_indicator) {
    auto& registry = HealthCheckRegistry::instance();

    // 清空之前的注册
    registry.unregister_health_indicator("test_app");

    auto indicator = std::make_unique<ApplicationHealthIndicator>();
    registry.register_health_indicator("test_app", std::move(indicator));

    Health health = registry.get_health("test_app");

    BOOST_CHECK(health.status == HealthStatus::UP);
}

BOOST_AUTO_TEST_CASE(test_health_registry_unregister_indicator) {
    auto& registry = HealthCheckRegistry::instance();

    registry.unregister_health_indicator("test_unregister");

    auto indicator = std::make_unique<ApplicationHealthIndicator>();
    registry.register_health_indicator("test_unregister", std::move(indicator));

    // 确保注册成功
    Health health1 = registry.get_health("test_unregister");
    BOOST_CHECK(health1.status == HealthStatus::UP);

    // 注销
    registry.unregister_health_indicator("test_unregister");

    // 注销后应该找不到
    Health health2 = registry.get_health("test_unregister");
    BOOST_CHECK(health2.status == HealthStatus::UNKNOWN);
}

BOOST_AUTO_TEST_CASE(test_health_registry_get_overall_health) {
    auto& registry = HealthCheckRegistry::instance();

    // 清理
    registry.unregister_health_indicator("test_overall1");
    registry.unregister_health_indicator("test_overall2");

    auto indicator1 = std::make_unique<ApplicationHealthIndicator>();
    auto indicator2 = std::make_unique<ApplicationHealthIndicator>();

    registry.register_health_indicator("test_overall1", std::move(indicator1));
    registry.register_health_indicator("test_overall2", std::move(indicator2));

    Health overall = registry.get_overall_health();

    BOOST_CHECK(overall.status == HealthStatus::UP);
    BOOST_CHECK(overall.is_healthy());
}

BOOST_AUTO_TEST_CASE(test_health_registry_get_all_health) {
    auto& registry = HealthCheckRegistry::instance();

    // 清理
    registry.unregister_health_indicator("test_all1");

    auto indicator = std::make_unique<ApplicationHealthIndicator>();
    registry.register_health_indicator("test_all1", std::move(indicator));

    auto all_health = registry.get_all_health();

    BOOST_CHECK(all_health.find("test_all1") != all_health.end());
    BOOST_CHECK(all_health["test_all1"].status == HealthStatus::UP);
}

BOOST_AUTO_TEST_CASE(test_health_registry_indicator_enabled) {
    auto& registry = HealthCheckRegistry::instance();

    registry.unregister_health_indicator("test_enabled");

    auto indicator = std::make_unique<ApplicationHealthIndicator>();
    registry.register_health_indicator("test_enabled", std::move(indicator));

    // 默认启用
    BOOST_CHECK(registry.is_indicator_enabled("test_enabled"));

    // 禁用
    registry.set_indicator_enabled("test_enabled", false);
    BOOST_CHECK(!registry.is_indicator_enabled("test_enabled"));

    // 重新启用
    registry.set_indicator_enabled("test_enabled", true);
    BOOST_CHECK(registry.is_indicator_enabled("test_enabled"));
}

BOOST_AUTO_TEST_CASE(test_health_registry_stats) {
    auto& registry = HealthCheckRegistry::instance();

    registry.unregister_health_indicator("test_stats");
    registry.clear_health_stats();

    auto indicator = std::make_unique<ApplicationHealthIndicator>();
    registry.register_health_indicator("test_stats", std::move(indicator));

    // 执行健康检查
    registry.get_health("test_stats");

    auto stats = registry.get_health_stats();

    BOOST_CHECK_GT(stats.total_checks, 0);
    BOOST_CHECK_GT(stats.healthy_checks, 0);
}

// =====================================
// Custom HealthIndicator 测试
// =====================================

class CustomHealthIndicator : public HealthIndicator {
public:
    CustomHealthIndicator(bool healthy) : healthy_(healthy) {}

    Health check() override {
        if (healthy_) {
            return Health(HealthStatus::UP, "Custom check passed")
                .add_detail("custom_field", "custom_value");
        } else {
            return Health(HealthStatus::DOWN, "Custom check failed")
                .add_detail("error", "Something went wrong");
        }
    }

    std::string name() const override { return "custom"; }

private:
    bool healthy_;
};

BOOST_AUTO_TEST_CASE(test_custom_health_indicator_success) {
    CustomHealthIndicator indicator(true);

    Health health = indicator.check();

    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
    BOOST_CHECK_EQUAL(health.description, "Custom check passed");
    BOOST_CHECK_EQUAL(health.details["custom_field"], "custom_value");
}

BOOST_AUTO_TEST_CASE(test_custom_health_indicator_failure) {
    CustomHealthIndicator indicator(false);

    Health health = indicator.check();

    BOOST_CHECK_EQUAL(health.status, HealthStatus::DOWN);
    BOOST_CHECK_EQUAL(health.description, "Custom check failed");
    BOOST_CHECK_EQUAL(health.details["error"], "Something went wrong");
}

// =====================================
// HealthEndpointBuilder 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_health_endpoint_builder_response) {
    Health overall(HealthStatus::UP, "Overall status");
    overall.add_detail("uptime", "100");

    std::unordered_map<std::string, Health> individual;
    individual["app"] = Health(HealthStatus::UP, "App is healthy");

    std::string response = HealthEndpointBuilder::build_health_response(
        overall, individual, true);

    BOOST_CHECK(!response.empty());
    BOOST_CHECK(response.find("UP") != std::string::npos);
    BOOST_CHECK(response.find("Overall status") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_health_endpoint_builder_json) {
    Health overall(HealthStatus::UP, "Overall status");
    overall.add_detail("uptime", "100");

    std::unordered_map<std::string, Health> individual;
    individual["app"] = Health(HealthStatus::UP, "App is healthy");

    std::string json = HealthEndpointBuilder::build_json_response(
        overall, individual, true);

    BOOST_CHECK(!json.empty());
    BOOST_CHECK(json.find("\"status\": \"UP\"") != std::string::npos);
    BOOST_CHECK(json.find("\"description\": \"Overall status\"") != std::string::npos);
    BOOST_CHECK(json.find("\"details\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_health_endpoint_builder_no_details) {
    Health overall(HealthStatus::UP, "Overall status");

    std::unordered_map<std::string, Health> individual;

    std::string response = HealthEndpointBuilder::build_health_response(
        overall, individual, false);

    BOOST_CHECK(response.find("Overall status") != std::string::npos);
    BOOST_CHECK(response.find("Details:") == std::string::npos);
}

// =====================================
// ReactiveHealthIndicator 测试
// =====================================

class AsyncHealthIndicator : public ReactiveHealthIndicator {
public:
    std::future<Health> check_async() override {
        return std::async(std::launch::async, []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return Health(HealthStatus::UP, "Async check completed");
        });
    }

    std::string name() const override { return "async"; }
};

BOOST_AUTO_TEST_CASE(test_reactive_health_indicator) {
    AsyncHealthIndicator indicator;

    Health health = indicator.check();

    BOOST_CHECK_EQUAL(health.status, HealthStatus::UP);
    BOOST_CHECK_EQUAL(health.description, "Async check completed");
}

BOOST_AUTO_TEST_CASE(test_reactive_health_indicator_timeout) {
    // 创建一个超时的异步指示器
    class SlowIndicator : public ReactiveHealthIndicator {
    public:
        std::future<Health> check_async() override {
            return std::async(std::launch::async, []() {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                return Health(HealthStatus::UP, "Should not reach here");
            });
        }

        std::string name() const override { return "slow"; }

        std::chrono::milliseconds timeout() const override {
            return std::chrono::milliseconds(100);
        }
    };

    SlowIndicator indicator;
    Health health = indicator.check();

    BOOST_CHECK_EQUAL(health.status, HealthStatus::DOWN);
    BOOST_CHECK(health.description.find("timed out") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
