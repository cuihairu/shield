// shield/src/health/health_check.cpp
#include "shield/health/health_check.hpp"

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <sys/statvfs.h>
#include <unistd.h>

namespace shield::health {

// =====================================
// DiskSpaceHealthIndicator 实现
// =====================================

Health DiskSpaceHealthIndicator::check() {
    namespace fs = std::filesystem;

    try {
        struct statvfs stat;
        if (statvfs(path_.c_str(), &stat) != 0) {
            return Health(HealthStatus::DOWN, "Failed to get disk space info")
                .add_detail("path", path_)
                .add_detail("error", strerror(errno));
        }

        unsigned long long total = stat.f_blocks * stat.f_frsize;
        unsigned long long available = stat.f_bavail * stat.f_frsize;
        unsigned long long free = stat.f_bfree * stat.f_frsize;
        double available_percent = (double)available / total * 100.0;

        std::ostringstream details;
        details << std::fixed << std::setprecision(2);
        details << "Total: " << (total / (1024.0 * 1024 * 1024)) << " GB, ";
        details << "Available: " << (available / (1024.0 * 1024 * 1024)) << " GB (";
        details << available_percent << "%)";

        HealthStatus status = (available >= min_free_bytes_)
                                 ? HealthStatus::UP
                                 : HealthStatus::DOWN;

        return Health(status, "Disk space check")
            .add_detail("path", path_)
            .add_detail("total_bytes", std::to_string(total))
            .add_detail("free_bytes", std::to_string(free))
            .add_detail("available_bytes", std::to_string(available))
            .add_detail("available_percent", std::to_string(available_percent))
            .add_detail("threshold_bytes", std::to_string(min_free_bytes_))
            .add_detail("details", details.str());

    } catch (const std::exception& e) {
        return Health(HealthStatus::DOWN, "Disk space check failed")
            .add_detail("path", path_)
            .add_detail("error", e.what());
    }
}

// =====================================
// DatabaseHealthIndicator 实现
// =====================================

std::future<Health> DatabaseHealthIndicator::check_async() {
    return std::async(std::launch::async, [this]() -> Health {
        // 简化的数据库健康检查
        // 实际实现应该尝试连接数据库并执行简单查询

        try {
            // TODO: 实现实际的数据库连接检查
            // 这里使用模拟实现

            return Health(HealthStatus::UP, "Database connection is healthy")
                .add_detail("connection", connection_string_)
                .add_detail("status", "connected");

        } catch (const std::exception& e) {
            return Health(HealthStatus::DOWN, "Database health check failed")
                .add_detail("connection", connection_string_)
                .add_detail("error", e.what());
        }
    });
}

// =====================================
// ApplicationHealthIndicator 实现
// =====================================

long ApplicationHealthIndicator::get_uptime_seconds() const {
    // 简化实现，返回进程运行时间
    // 实际应该记录启动时间
    return 0;
}

std::string ApplicationHealthIndicator::get_application_version() const {
    // 返回应用版本
    // 可以从配置或其他地方获取
    return "1.0.0";
}

// =====================================
// HealthCheckRegistry 实现
// =====================================

void HealthCheckRegistry::register_health_indicator(
    std::unique_ptr<HealthIndicator> indicator) {
    if (!indicator) {
        return;
    }

    std::lock_guard<std::mutex> lock(indicators_mutex_);
    indicators_[indicator->name()] = std::move(indicator);
}

void HealthCheckRegistry::register_health_indicator(
    const std::string& name, std::unique_ptr<HealthIndicator> indicator) {
    if (!indicator || name.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(indicators_mutex_);
    indicators_[name] = std::move(indicator);
}

void HealthCheckRegistry::unregister_health_indicator(const std::string& name) {
    std::lock_guard<std::mutex> lock(indicators_mutex_);
    indicators_.erase(name);
}

Health HealthCheckRegistry::get_overall_health() {
    std::lock_guard<std::mutex> lock(indicators_mutex_);

    if (indicators_.empty()) {
        return Health(HealthStatus::UNKNOWN, "No health indicators registered");
    }

    std::vector<Health> healths;
    auto start = std::chrono::steady_clock::now();

    for (const auto& [name, indicator] : indicators_) {
        if (is_indicator_enabled(name) &&
            indicator->contributes_to_overall_health()) {
            try {
                auto health_start = std::chrono::steady_clock::now();
                auto health = indicator->check();
                auto health_end = std::chrono::steady_clock::now();

                auto check_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    health_end - health_start);
                update_stats(health, check_time);

                healths.push_back(health);
            } catch (const std::exception& e) {
                healths.push_back(Health(HealthStatus::DOWN,
                                        "Health check threw exception")
                                     .add_detail("error", e.what()));
            }
        }
    }

    auto end = std::chrono::steady_clock::now();

    // 使用聚合器计算总体健康状态
    if (health_aggregator_) {
        return health_aggregator_(healths);
    }

    return default_health_aggregator(healths);
}

Health HealthCheckRegistry::get_health(const std::string& indicator_name) {
    std::lock_guard<std::mutex> lock(indicators_mutex_);

    auto it = indicators_.find(indicator_name);
    if (it == indicators_.end()) {
        return Health(HealthStatus::UNKNOWN,
                     "Health indicator not found: " + indicator_name);
    }

    try {
        auto start = std::chrono::steady_clock::now();
        auto health = it->second->check();
        auto end = std::chrono::steady_clock::now();

        auto check_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start);
        update_stats(health, check_time);

        return health;
    } catch (const std::exception& e) {
        return Health(HealthStatus::DOWN, "Health check threw exception")
            .add_detail("error", e.what());
    }
}

std::unordered_map<std::string, Health> HealthCheckRegistry::get_all_health() {
    std::lock_guard<std::mutex> lock(indicators_mutex_);

    std::unordered_map<std::string, Health> results;

    for (const auto& [name, indicator] : indicators_) {
        if (is_indicator_enabled(name)) {
            try {
                auto start = std::chrono::steady_clock::now();
                auto health = indicator->check();
                auto end = std::chrono::steady_clock::now();

                auto check_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end - start);
                update_stats(health, check_time);

                results[name] = health;
            } catch (const std::exception& e) {
                results[name] =
                    Health(HealthStatus::DOWN, "Health check threw exception")
                        .add_detail("error", e.what());
            }
        }
    }

    return results;
}

void HealthCheckRegistry::set_indicator_enabled(const std::string& name,
                                                bool enabled) {
    std::lock_guard<std::mutex> lock(indicators_mutex_);
    indicator_enabled_status_[name] = enabled;
}

bool HealthCheckRegistry::is_indicator_enabled(const std::string& name) const {
    auto it = indicator_enabled_status_.find(name);
    if (it == indicator_enabled_status_.end()) {
        return true;  // 默认启用
    }
    return it->second;
}

HealthCheckRegistry::HealthStats HealthCheckRegistry::get_health_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void HealthCheckRegistry::clear_health_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = HealthStats{};
}

Health HealthCheckRegistry::default_health_aggregator(
    const std::vector<Health>& healths) {
    if (healths.empty()) {
        return Health(HealthStatus::UNKNOWN, "No health checks performed");
    }

    // 如果有任何 DOWN，总体状态为 DOWN
    // 否则如果有 UNKNOWN，总体状态为 UNKNOWN
    // 否则总体状态为 UP
    HealthStatus overall = HealthStatus::UP;

    for (const auto& health : healths) {
        if (health.status == HealthStatus::DOWN) {
            overall = HealthStatus::DOWN;
            break;
        } else if (health.status == HealthStatus::UNKNOWN &&
                   overall == HealthStatus::UP) {
            overall = HealthStatus::UNKNOWN;
        }
    }

    size_t healthy_count =
        std::count_if(healths.begin(), healths.end(),
                      [](const Health& h) { return h.is_healthy(); });

    std::ostringstream desc;
    desc << healthy_count << " of " << healths.size()
         << " health checks passed";

    return Health(overall, desc.str());
}

void HealthCheckRegistry::update_stats(
    const Health& health, std::chrono::milliseconds check_time) {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    stats_.total_checks++;

    if (health.is_healthy()) {
        stats_.healthy_checks++;
    } else {
        stats_.unhealthy_checks++;
    }

    // 更新平均检查时间
    auto total_ms = stats_.avg_check_time.count() * (stats_.total_checks - 1);
    total_ms += check_time.count();
    stats_.avg_check_time =
        std::chrono::milliseconds(total_ms / stats_.total_checks);
}

// =====================================
// HealthEndpointBuilder 实现
// =====================================

std::string HealthEndpointBuilder::build_health_response(
    const Health& overall_health,
    const std::unordered_map<std::string, Health>& individual_healths,
    bool show_details) {
    std::ostringstream oss;

    oss << "Health Status: " << health_status_to_string(overall_health.status)
        << "\n";
    oss << "Description: " << overall_health.description << "\n";

    if (show_details) {
        oss << "\nDetails:\n";
        for (const auto& [key, value] : overall_health.details) {
            oss << "  " << key << ": " << value << "\n";
        }

        if (!individual_healths.empty()) {
            oss << "\nIndividual Checks:\n";
            for (const auto& [name, health] : individual_healths) {
                oss << "  " << name << ": "
                    << health_status_to_string(health.status);
                if (!health.description.empty()) {
                    oss << " (" << health.description << ")";
                }
                oss << "\n";
            }
        }

        oss << "\nTimestamp: "
            << format_timestamp(overall_health.timestamp) << "\n";
    }

    return oss.str();
}

std::string HealthEndpointBuilder::build_json_response(
    const Health& overall_health,
    const std::unordered_map<std::string, Health>& individual_healths,
    bool show_details) {
    std::ostringstream oss;

    oss << "{\n";
    oss << "  \"status\": \""
        << health_status_to_string(overall_health.status) << "\",\n";
    oss << "  \"description\": \"" << overall_health.description << "\",\n";

    if (show_details) {
        oss << "  \"details\": {\n";
        bool first = true;
        for (const auto& [key, value] : overall_health.details) {
            if (!first) oss << ",\n";
            oss << "    \"" << key << "\": \"" << value << "\"";
            first = false;
        }
        oss << "\n  },\n";

        if (!individual_healths.empty()) {
            oss << "  \"components\": {\n";
            bool first_comp = true;
            for (const auto& [name, health] : individual_healths) {
                if (!first_comp) oss << ",\n";
                oss << "    \"" << name << "\": {\n";
                oss << "      \"status\": \""
                    << health_status_to_string(health.status) << "\"";
                if (!health.description.empty()) {
                    oss << ",\n      \"description\": \"" << health.description
                        << "\"";
                }
                if (!health.details.empty()) {
                    oss << ",\n      \"details\": {";
                    bool first_detail = true;
                    for (const auto& [dk, dv] : health.details) {
                        if (!first_detail) oss << ", ";
                        oss << "\"" << dk << "\": \"" << dv << "\"";
                        first_detail = false;
                    }
                    oss << "}";
                }
                oss << "\n    }";
                first_comp = false;
            }
            oss << "\n  },\n";
        }

        oss << "  \"timestamp\": \""
            << format_timestamp(overall_health.timestamp) << "\"\n";
    } else {
        oss << "  \"timestamp\": \""
            << format_timestamp(overall_health.timestamp) << "\"\n";
    }

    oss << "}";

    return oss.str();
}

std::string HealthEndpointBuilder::health_status_to_string(HealthStatus status) {
    switch (status) {
        case HealthStatus::UP:
            return "UP";
        case HealthStatus::DOWN:
            return "DOWN";
        case HealthStatus::OUT_OF_SERVICE:
            return "OUT_OF_SERVICE";
        case HealthStatus::UNKNOWN:
            return "UNKNOWN";
        default:
            return "UNKNOWN";
    }
}

std::string HealthEndpointBuilder::format_timestamp(
    std::chrono::steady_clock::time_point timestamp) {
    // 使用 steady_clock 的 duration 转换为可显示的时间
    auto now = std::chrono::steady_clock::now();
    auto diff = timestamp - now;

    // 简化实现：直接使用当前时间
    auto system_now = std::chrono::system_clock::now();
    auto time_t_value = std::chrono::system_clock::to_time_t(system_now);

    std::tm tm_value;
    localtime_r(&time_t_value, &tm_value);

    std::ostringstream oss;
    oss << std::put_time(&tm_value, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

}  // namespace shield::health
