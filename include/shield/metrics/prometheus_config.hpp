#pragma once

#include <cstdint>
#include <string>

#include "shield/config/config.hpp"

namespace shield::metrics {

// Prometheus module configuration
class PrometheusConfig
    : public config::ReloadableConfigurationProperties<PrometheusConfig> {
public:
    // Basic configuration
    struct ServerConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 9090;
        std::string path = "/metrics";
        int max_connections = 100;
    };

    // System metrics configuration
    struct SystemMetricsConfig {
        bool enabled = true;
        int collection_interval = 5;  // seconds
        bool collect_cpu = true;
        bool collect_memory = true;
        bool collect_disk = true;
        bool collect_network = false;
    };

    // Application metrics configuration
    struct AppMetricsConfig {
        bool enabled = true;
        bool collect_http_requests = true;
        bool collect_actor_stats = true;
        bool collect_gateway_stats = true;
        bool collect_lua_stats = true;
    };

    // Metrics export configuration
    struct ExportConfig {
        std::string format = "prometheus";  // prometheus, json, etc.
        bool include_timestamp = true;
        bool include_help_text = true;
        std::string namespace_prefix = "shield";
    };

    // Configuration data
    ServerConfig server;
    SystemMetricsConfig system_metrics;
    AppMetricsConfig app_metrics;
    ExportConfig export_config;

    // ComponentConfig interface implementation
    void from_ptree(const boost::property_tree::ptree& pt) override;
    void validate() const override;
    std::string properties_name() const override { return "prometheus"; }

    // Convenience methods
    bool is_metrics_enabled() const { return server.enabled; }
    std::string get_metrics_endpoint() const;
};

}  // namespace shield::metrics