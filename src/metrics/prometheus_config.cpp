#include "shield/metrics/prometheus_config.hpp"

#include <stdexcept>

#include "shield/log/logger.hpp"

namespace shield::metrics {

void PrometheusConfig::from_ptree(const boost::property_tree::ptree& pt) {
    // Server configuration
    if (auto server_pt = pt.get_child_optional("server")) {
        server.enabled = get_value(*server_pt, "enabled", server.enabled);
        server.host = get_value(*server_pt, "host", server.host);
        server.port = get_value(*server_pt, "port", server.port);
        server.path = get_value(*server_pt, "path", server.path);
        server.max_connections =
            get_value(*server_pt, "max_connections", server.max_connections);
    }

    // System metrics configuration
    if (auto sys_pt = pt.get_child_optional("system_metrics")) {
        system_metrics.enabled =
            get_value(*sys_pt, "enabled", system_metrics.enabled);
        system_metrics.collection_interval = get_value(
            *sys_pt, "collection_interval", system_metrics.collection_interval);
        system_metrics.collect_cpu =
            get_value(*sys_pt, "collect_cpu", system_metrics.collect_cpu);
        system_metrics.collect_memory =
            get_value(*sys_pt, "collect_memory", system_metrics.collect_memory);
        system_metrics.collect_disk =
            get_value(*sys_pt, "collect_disk", system_metrics.collect_disk);
        system_metrics.collect_network = get_value(
            *sys_pt, "collect_network", system_metrics.collect_network);
    }

    // App metrics configuration
    if (auto app_pt = pt.get_child_optional("app_metrics")) {
        app_metrics.enabled =
            get_value(*app_pt, "enabled", app_metrics.enabled);
        app_metrics.collect_http_requests =
            get_value(*app_pt, "collect_http_requests",
                      app_metrics.collect_http_requests);
        app_metrics.collect_actor_stats = get_value(
            *app_pt, "collect_actor_stats", app_metrics.collect_actor_stats);
        app_metrics.collect_gateway_stats =
            get_value(*app_pt, "collect_gateway_stats",
                      app_metrics.collect_gateway_stats);
        app_metrics.collect_lua_stats = get_value(
            *app_pt, "collect_lua_stats", app_metrics.collect_lua_stats);
    }

    // Export configuration
    if (auto export_pt = pt.get_child_optional("export")) {
        export_config.format =
            get_value(*export_pt, "format", export_config.format);
        export_config.include_timestamp = get_value(
            *export_pt, "include_timestamp", export_config.include_timestamp);
        export_config.include_help_text = get_value(
            *export_pt, "include_help_text", export_config.include_help_text);
        export_config.namespace_prefix = get_value(
            *export_pt, "namespace_prefix", export_config.namespace_prefix);
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