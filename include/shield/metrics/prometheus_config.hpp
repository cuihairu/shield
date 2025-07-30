#pragma once

#include <cstdint>
#include <string>

#include "shield/config/config.hpp"

namespace shield::metrics {

// Prometheus模块配置
class PrometheusConfig : public config::ComponentConfig {
public:
    // 基础配置
    struct ServerConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 9090;
        std::string path = "/metrics";
        int max_connections = 100;
    };

    // 系统指标配置
    struct SystemMetricsConfig {
        bool enabled = true;
        int collection_interval = 5;  // seconds
        bool collect_cpu = true;
        bool collect_memory = true;
        bool collect_disk = true;
        bool collect_network = false;
    };

    // 应用指标配置
    struct AppMetricsConfig {
        bool enabled = true;
        bool collect_http_requests = true;
        bool collect_actor_stats = true;
        bool collect_gateway_stats = true;
        bool collect_lua_stats = true;
    };

    // 指标导出配置
    struct ExportConfig {
        std::string format = "prometheus";  // prometheus, json, etc.
        bool include_timestamp = true;
        bool include_help_text = true;
        std::string namespace_prefix = "shield";
    };

    // 配置数据
    ServerConfig server;
    SystemMetricsConfig system_metrics;
    AppMetricsConfig app_metrics;
    ExportConfig export_config;

    // ComponentConfig接口实现
    void from_ptree(const boost::property_tree::ptree& pt) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string component_name() const override { return "prometheus"; }
    CLONE_IMPL(PrometheusConfig)

    // 便利方法
    bool is_metrics_enabled() const { return server.enabled; }
    std::string get_metrics_endpoint() const;
};

}  // namespace shield::metrics