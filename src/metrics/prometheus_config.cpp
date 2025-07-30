#include "shield/metrics/prometheus_config.hpp"

#include <stdexcept>

#include "shield/log/logger.hpp"

namespace shield::metrics {

void PrometheusConfig::from_yaml(const YAML::Node& node) {
    if (node["server"]) {
        auto server_node = node["server"];
        if (server_node["enabled"])
            server.enabled = server_node["enabled"].as<bool>();
        if (server_node["host"])
            server.host = server_node["host"].as<std::string>();
        if (server_node["port"])
            server.port = server_node["port"].as<uint16_t>();
        if (server_node["path"])
            server.path = server_node["path"].as<std::string>();
        if (server_node["max_connections"])
            server.max_connections = server_node["max_connections"].as<int>();
    }

    if (node["system_metrics"]) {
        auto sys_node = node["system_metrics"];
        if (sys_node["enabled"])
            system_metrics.enabled = sys_node["enabled"].as<bool>();
        if (sys_node["collection_interval"])
            system_metrics.collection_interval =
                sys_node["collection_interval"].as<int>();
        if (sys_node["collect_cpu"])
            system_metrics.collect_cpu = sys_node["collect_cpu"].as<bool>();
        if (sys_node["collect_memory"])
            system_metrics.collect_memory =
                sys_node["collect_memory"].as<bool>();
        if (sys_node["collect_disk"])
            system_metrics.collect_disk = sys_node["collect_disk"].as<bool>();
        if (sys_node["collect_network"])
            system_metrics.collect_network =
                sys_node["collect_network"].as<bool>();
    }

    if (node["app_metrics"]) {
        auto app_node = node["app_metrics"];
        if (app_node["enabled"])
            app_metrics.enabled = app_node["enabled"].as<bool>();
        if (app_node["collect_http_requests"])
            app_metrics.collect_http_requests =
                app_node["collect_http_requests"].as<bool>();
        if (app_node["collect_actor_stats"])
            app_metrics.collect_actor_stats =
                app_node["collect_actor_stats"].as<bool>();
        if (app_node["collect_gateway_stats"])
            app_metrics.collect_gateway_stats =
                app_node["collect_gateway_stats"].as<bool>();
        if (app_node["collect_lua_stats"])
            app_metrics.collect_lua_stats =
                app_node["collect_lua_stats"].as<bool>();
    }

    if (node["export"]) {
        auto export_node = node["export"];
        if (export_node["format"])
            export_config.format = export_node["format"].as<std::string>();
        if (export_node["include_timestamp"])
            export_config.include_timestamp =
                export_node["include_timestamp"].as<bool>();
        if (export_node["include_help_text"])
            export_config.include_help_text =
                export_node["include_help_text"].as<bool>();
        if (export_node["namespace_prefix"])
            export_config.namespace_prefix =
                export_node["namespace_prefix"].as<std::string>();
    }
}

YAML::Node PrometheusConfig::to_yaml() const {
    YAML::Node node;

    // Server配置
    node["server"]["enabled"] = server.enabled;
    node["server"]["host"] = server.host;
    node["server"]["port"] = server.port;
    node["server"]["path"] = server.path;
    node["server"]["max_connections"] = server.max_connections;

    // 系统指标配置
    node["system_metrics"]["enabled"] = system_metrics.enabled;
    node["system_metrics"]["collection_interval"] =
        system_metrics.collection_interval;
    node["system_metrics"]["collect_cpu"] = system_metrics.collect_cpu;
    node["system_metrics"]["collect_memory"] = system_metrics.collect_memory;
    node["system_metrics"]["collect_disk"] = system_metrics.collect_disk;
    node["system_metrics"]["collect_network"] = system_metrics.collect_network;

    // 应用指标配置
    node["app_metrics"]["enabled"] = app_metrics.enabled;
    node["app_metrics"]["collect_http_requests"] =
        app_metrics.collect_http_requests;
    node["app_metrics"]["collect_actor_stats"] =
        app_metrics.collect_actor_stats;
    node["app_metrics"]["collect_gateway_stats"] =
        app_metrics.collect_gateway_stats;
    node["app_metrics"]["collect_lua_stats"] = app_metrics.collect_lua_stats;

    // 导出配置
    node["export"]["format"] = export_config.format;
    node["export"]["include_timestamp"] = export_config.include_timestamp;
    node["export"]["include_help_text"] = export_config.include_help_text;
    node["export"]["namespace_prefix"] = export_config.namespace_prefix;

    return node;
}

void PrometheusConfig::validate() const {
    if (server.enabled) {
        if (server.host.empty()) {
            throw std::invalid_argument(
                "Prometheus server host cannot be empty when enabled");
        }

        if (server.port == 0) {
            throw std::invalid_argument(
                "Prometheus server port must be greater than 0");
        }

        if (server.path.empty()) {
            throw std::invalid_argument(
                "Prometheus metrics path cannot be empty");
        }

        if (!server.path.starts_with("/")) {
            throw std::invalid_argument(
                "Prometheus metrics path must start with '/'");
        }

        if (server.max_connections <= 0) {
            throw std::invalid_argument(
                "Prometheus max connections must be greater than 0");
        }
    }

    if (system_metrics.enabled && system_metrics.collection_interval <= 0) {
        throw std::invalid_argument(
            "System metrics collection interval must be greater than 0");
    }

    if (export_config.format != "prometheus" &&
        export_config.format != "json") {
        throw std::invalid_argument(
            "Export format must be 'prometheus' or 'json'");
    }

    if (export_config.namespace_prefix.empty()) {
        throw std::invalid_argument("Namespace prefix cannot be empty");
    }
}

std::string PrometheusConfig::get_metrics_endpoint() const {
    return "http://" + server.host + ":" + std::to_string(server.port) +
           server.path;
}

}  // namespace shield::metrics