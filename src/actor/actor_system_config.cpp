#include "shield/actor/actor_system_config.hpp"

#include <unistd.h>  // for gethostname(), getpid()

#include <chrono>
#include <stdexcept>
#include <thread>

#include "shield/log/logger.hpp"

namespace shield::actor {

void ActorSystemConfig::from_yaml(const YAML::Node& node_yaml) {
    if (node_yaml["node"]) {
        auto node_config = node_yaml["node"];
        if (node_config["node_id"])
            node.node_id = node_config["node_id"].as<std::string>();
        if (node_config["cluster_name"])
            node.cluster_name = node_config["cluster_name"].as<std::string>();
        if (node_config["auto_generate_node_id"])
            node.auto_generate_node_id =
                node_config["auto_generate_node_id"].as<bool>();
    }

    if (node_yaml["scheduler"]) {
        auto scheduler_config = node_yaml["scheduler"];
        if (scheduler_config["policy"])
            scheduler.policy = scheduler_config["policy"].as<std::string>();
        if (scheduler_config["worker_threads"])
            scheduler.worker_threads =
                scheduler_config["worker_threads"].as<int>();
        if (scheduler_config["max_throughput"])
            scheduler.max_throughput =
                scheduler_config["max_throughput"].as<int>();
        if (scheduler_config["enable_profiling"])
            scheduler.enable_profiling =
                scheduler_config["enable_profiling"].as<bool>();
    }

    if (node_yaml["network"]) {
        auto network_config = node_yaml["network"];
        if (network_config["enabled"])
            network.enabled = network_config["enabled"].as<bool>();
        if (network_config["host"])
            network.host = network_config["host"].as<std::string>();
        if (network_config["port"])
            network.port = network_config["port"].as<uint16_t>();
        if (network_config["max_connections"])
            network.max_connections =
                network_config["max_connections"].as<int>();
        if (network_config["connection_timeout"])
            network.connection_timeout =
                network_config["connection_timeout"].as<int>();
    }

    if (node_yaml["monitor"]) {
        auto monitor_config = node_yaml["monitor"];
        if (monitor_config["enable_metrics"])
            monitor.enable_metrics =
                monitor_config["enable_metrics"].as<bool>();
        if (monitor_config["enable_tracing"])
            monitor.enable_tracing =
                monitor_config["enable_tracing"].as<bool>();
        if (monitor_config["metrics_interval"])
            monitor.metrics_interval =
                monitor_config["metrics_interval"].as<int>();
        if (monitor_config["metrics_output"])
            monitor.metrics_output =
                monitor_config["metrics_output"].as<std::string>();
    }

    if (node_yaml["memory"]) {
        auto memory_config = node_yaml["memory"];
        if (memory_config["max_memory_per_actor"])
            memory.max_memory_per_actor =
                memory_config["max_memory_per_actor"].as<size_t>();
        if (memory_config["message_buffer_size"])
            memory.message_buffer_size =
                memory_config["message_buffer_size"].as<size_t>();
        if (memory_config["enable_gc"])
            memory.enable_gc = memory_config["enable_gc"].as<bool>();
        if (memory_config["gc_interval"])
            memory.gc_interval = memory_config["gc_interval"].as<int>();
    }
}

YAML::Node ActorSystemConfig::to_yaml() const {
    YAML::Node node_yaml;

    // Node配置
    node_yaml["node"]["node_id"] = node.node_id;
    node_yaml["node"]["cluster_name"] = node.cluster_name;
    node_yaml["node"]["auto_generate_node_id"] = node.auto_generate_node_id;

    // Scheduler配置
    node_yaml["scheduler"]["policy"] = scheduler.policy;
    node_yaml["scheduler"]["worker_threads"] = scheduler.worker_threads;
    node_yaml["scheduler"]["max_throughput"] = scheduler.max_throughput;
    node_yaml["scheduler"]["enable_profiling"] = scheduler.enable_profiling;

    // Network配置
    node_yaml["network"]["enabled"] = network.enabled;
    node_yaml["network"]["host"] = network.host;
    node_yaml["network"]["port"] = network.port;
    node_yaml["network"]["max_connections"] = network.max_connections;
    node_yaml["network"]["connection_timeout"] = network.connection_timeout;

    // Monitor配置
    node_yaml["monitor"]["enable_metrics"] = monitor.enable_metrics;
    node_yaml["monitor"]["enable_tracing"] = monitor.enable_tracing;
    node_yaml["monitor"]["metrics_interval"] = monitor.metrics_interval;
    node_yaml["monitor"]["metrics_output"] = monitor.metrics_output;

    // Memory配置
    node_yaml["memory"]["max_memory_per_actor"] = memory.max_memory_per_actor;
    node_yaml["memory"]["message_buffer_size"] = memory.message_buffer_size;
    node_yaml["memory"]["enable_gc"] = memory.enable_gc;
    node_yaml["memory"]["gc_interval"] = memory.gc_interval;

    return node_yaml;
}

void ActorSystemConfig::validate() const {
    if (node.node_id.empty() && !node.auto_generate_node_id) {
        throw std::invalid_argument(
            "Actor system node_id cannot be empty when auto_generate_node_id "
            "is false");
    }

    if (node.cluster_name.empty()) {
        throw std::invalid_argument(
            "Actor system cluster_name cannot be empty");
    }

    if (scheduler.policy != "sharing" && scheduler.policy != "stealing") {
        throw std::invalid_argument(
            "Actor system scheduler policy must be 'sharing' or 'stealing'");
    }

    if (scheduler.max_throughput <= 0) {
        throw std::invalid_argument(
            "Actor system max_throughput must be greater than 0");
    }

    if (network.enabled) {
        if (network.host.empty()) {
            throw std::invalid_argument(
                "Actor system network host cannot be empty when network is "
                "enabled");
        }

        if (network.max_connections <= 0) {
            throw std::invalid_argument(
                "Actor system max_connections must be greater than 0");
        }

        if (network.connection_timeout <= 0) {
            throw std::invalid_argument(
                "Actor system connection_timeout must be greater than 0");
        }
    }

    if (monitor.metrics_interval <= 0) {
        throw std::invalid_argument(
            "Actor system metrics_interval must be greater than 0");
    }

    if (memory.max_memory_per_actor == 0) {
        throw std::invalid_argument(
            "Actor system max_memory_per_actor must be greater than 0");
    }

    if (memory.message_buffer_size == 0) {
        throw std::invalid_argument(
            "Actor system message_buffer_size must be greater than 0");
    }
}

int ActorSystemConfig::get_effective_worker_threads() const {
    return (scheduler.worker_threads > 0)
               ? scheduler.worker_threads
               : static_cast<int>(std::thread::hardware_concurrency());
}

std::string ActorSystemConfig::get_effective_node_id() const {
    if (!node.auto_generate_node_id || !node.node_id.empty()) {
        return node.node_id;
    }

    // 生成基于主机名和PID的唯一节点ID
    std::string hostname = "unknown";
    char hostname_buffer[256];
    if (gethostname(hostname_buffer, sizeof(hostname_buffer)) == 0) {
        hostname = std::string(hostname_buffer);
    }

    auto process_id = getpid();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    return hostname + "_" + std::to_string(process_id) + "_" +
           std::to_string(timestamp);
}

}  // namespace shield::actor