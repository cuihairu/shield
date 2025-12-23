#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace shield::health {

/**
 * @brief Health status enumeration
 */
enum class HealthStatus {
    UP,              // Service is healthy
    DOWN,            // Service is unhealthy
    OUT_OF_SERVICE,  // Service is temporarily out of service
    UNKNOWN          // Health status cannot be determined
};

// 输出流操作符
inline std::ostream& operator<<(std::ostream& os, HealthStatus status) {
    switch (status) {
        case HealthStatus::UP:
            return os << "UP";
        case HealthStatus::DOWN:
            return os << "DOWN";
        case HealthStatus::OUT_OF_SERVICE:
            return os << "OUT_OF_SERVICE";
        case HealthStatus::UNKNOWN:
            return os << "UNKNOWN";
        default:
            return os << "UNKNOWN";
    }
}

/**
 * @brief Health check result
 */
struct Health {
    HealthStatus status;
    std::string description;
    std::unordered_map<std::string, std::string> details;
    std::chrono::steady_clock::time_point timestamp;

    Health(HealthStatus s = HealthStatus::UNKNOWN, const std::string& desc = "")
        : status(s),
          description(desc),
          timestamp(std::chrono::steady_clock::now()) {}

    Health& add_detail(const std::string& key, const std::string& value) {
        details[key] = value;
        return *this;
    }

    bool is_healthy() const { return status == HealthStatus::UP; }
};

/**
 * @brief Health indicator interface (similar to Spring Boot's HealthIndicator)
 */
class HealthIndicator {
public:
    virtual ~HealthIndicator() = default;

    /**
     * @brief Perform health check
     * @return Health check result
     */
    virtual Health check() = 0;

    /**
     * @brief Get indicator name
     */
    virtual std::string name() const = 0;

    /**
     * @brief Get health check timeout in milliseconds
     */
    virtual std::chrono::milliseconds timeout() const {
        return std::chrono::milliseconds(5000);  // 5 second default
    }

    /**
     * @brief Check if this indicator should be included in overall health
     */
    virtual bool contributes_to_overall_health() const { return true; }
};

/**
 * @brief Abstract reactive health indicator for async health checks
 */
class ReactiveHealthIndicator : public HealthIndicator {
public:
    /**
     * @brief Perform async health check
     */
    virtual std::future<Health> check_async() = 0;

    /**
     * @brief Synchronous check delegates to async version
     */
    Health check() override {
        auto future = check_async();
        if (future.wait_for(timeout()) == std::future_status::ready) {
            return future.get();
        }
        return Health(HealthStatus::DOWN, "Health check timed out");
    }
};

/**
 * @brief Built-in health indicators
 */

// Disk space health indicator
class DiskSpaceHealthIndicator : public HealthIndicator {
public:
    explicit DiskSpaceHealthIndicator(
        const std::string& path = "/",
        size_t min_free_bytes = 10 * 1024 * 1024)  // 10MB default
        : path_(path), min_free_bytes_(min_free_bytes) {}

    Health check() override;
    std::string name() const override { return "diskSpace"; }

private:
    std::string path_;
    size_t min_free_bytes_;
};

// Database health indicator
class DatabaseHealthIndicator : public ReactiveHealthIndicator {
public:
    explicit DatabaseHealthIndicator(const std::string& connection_string)
        : connection_string_(connection_string) {}

    std::future<Health> check_async() override;
    std::string name() const override { return "database"; }

private:
    std::string connection_string_;
};

// Application health indicator
class ApplicationHealthIndicator : public HealthIndicator {
public:
    Health check() override {
        return Health(HealthStatus::UP, "Application is running")
            .add_detail("uptime", std::to_string(get_uptime_seconds()))
            .add_detail("version", get_application_version());
    }

    std::string name() const override { return "application"; }

private:
    long get_uptime_seconds() const;
    std::string get_application_version() const;
};

/**
 * @brief Health check registry and endpoint (similar to Spring Boot Actuator)
 */
class HealthCheckRegistry {
public:
    static HealthCheckRegistry& instance() {
        static HealthCheckRegistry registry;
        return registry;
    }

    /**
     * @brief Register a health indicator
     */
    void register_health_indicator(std::unique_ptr<HealthIndicator> indicator);

    /**
     * @brief Register a health indicator with name
     */
    void register_health_indicator(const std::string& name,
                                   std::unique_ptr<HealthIndicator> indicator);

    /**
     * @brief Remove health indicator
     */
    void unregister_health_indicator(const std::string& name);

    /**
     * @brief Get overall health status
     */
    Health get_overall_health();

    /**
     * @brief Get health status for specific indicator
     */
    Health get_health(const std::string& indicator_name);

    /**
     * @brief Get all health indicators status
     */
    std::unordered_map<std::string, Health> get_all_health();

    /**
     * @brief Set health aggregation strategy
     */
    void set_health_aggregator(
        std::function<Health(const std::vector<Health>&)> aggregator) {
        health_aggregator_ = std::move(aggregator);
    }

    /**
     * @brief Enable/disable specific health indicator
     */
    void set_indicator_enabled(const std::string& name, bool enabled);

    /**
     * @brief Check if indicator is enabled
     */
    bool is_indicator_enabled(const std::string& name) const;

    /**
     * @brief Get health check statistics
     */
    struct HealthStats {
        size_t total_checks = 0;
        size_t healthy_checks = 0;
        size_t unhealthy_checks = 0;
        std::chrono::milliseconds avg_check_time{0};
    };

    HealthStats get_health_stats() const;
    void clear_health_stats();

private:
    HealthCheckRegistry() = default;

    std::unordered_map<std::string, std::unique_ptr<HealthIndicator>>
        indicators_;
    std::unordered_map<std::string, bool> indicator_enabled_status_;
    std::function<Health(const std::vector<Health>&)> health_aggregator_;

    mutable std::mutex indicators_mutex_;
    mutable std::mutex stats_mutex_;
    HealthStats stats_;

    // Default health aggregator
    Health default_health_aggregator(const std::vector<Health>& healths);
    void update_stats(const Health& health,
                      std::chrono::milliseconds check_time);
};

/**
 * @brief Health endpoint builder for HTTP exposure
 */
class HealthEndpointBuilder {
public:
    /**
     * @brief Build health check endpoint response
     */
    static std::string build_health_response(
        const Health& overall_health,
        const std::unordered_map<std::string, Health>& individual_healths,
        bool show_details = true);

    /**
     * @brief Build health response in JSON format
     */
    static std::string build_json_response(
        const Health& overall_health,
        const std::unordered_map<std::string, Health>& individual_healths,
        bool show_details = true);

private:
    static std::string health_status_to_string(HealthStatus status);
    static std::string format_timestamp(
        std::chrono::steady_clock::time_point timestamp);
};

}  // namespace shield::health

/**
 * @brief Health check macros for easy registration
 */
#define SHIELD_HEALTH_INDICATOR(ClassName)                             \
    static inline auto _shield_health_##ClassName = []() {             \
        shield::health::HealthCheckRegistry::instance()                \
            .register_health_indicator(std::make_unique<ClassName>()); \
        return 0;                                                      \
    }();

#define SHIELD_HEALTH_INDICATOR_NAMED(ClassName, name)                        \
    static inline auto _shield_health_##ClassName##_##name = []() {           \
        shield::health::HealthCheckRegistry::instance()                       \
            .register_health_indicator(#name, std::make_unique<ClassName>()); \
        return 0;                                                             \
    }();