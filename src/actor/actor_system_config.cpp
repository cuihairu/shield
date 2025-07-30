#include "shield/actor/actor_system_config.hpp"

#include <unistd.h>  // for gethostname(), getpid()

#include <chrono>
#include <stdexcept>
#include <thread>

#include "shield/log/logger.hpp"

namespace shield::actor {

void ActorSystemConfig::from_ptree(const boost::property_tree::ptree& pt) {
    // Node configuration
    if (auto node_pt = pt.get_child_optional("node")) {
        node.node_id = get_value(*node_pt, "node_id", node.node_id);
        node.cluster_name =
            get_value(*node_pt, "cluster_name", node.cluster_name);
        node.auto_generate_node_id = get_value(
            *node_pt, "auto_generate_node_id", node.auto_generate_node_id);
    }

    // Scheduler configuration
    if (auto scheduler_pt = pt.get_child_optional("scheduler")) {
        scheduler.policy = get_value(*scheduler_pt, "policy", scheduler.policy);
        scheduler.worker_threads = get_value(*scheduler_pt, "worker_threads",
                                             scheduler.worker_threads);
        scheduler.max_throughput = get_value(*scheduler_pt, "max_throughput",
                                             scheduler.max_throughput);
        scheduler.enable_profiling = get_value(
            *scheduler_pt, "enable_profiling", scheduler.enable_profiling);
    }

    // Network configuration
    if (auto network_pt = pt.get_child_optional("network")) {
        network.enabled = get_value(*network_pt, "enabled", network.enabled);
        network.host = get_value(*network_pt, "host", network.host);
        network.port = get_value(*network_pt, "port", network.port);
        network.max_connections =
            get_value(*network_pt, "max_connections", network.max_connections);
        network.connection_timeout = get_value(
            *network_pt, "connection_timeout", network.connection_timeout);
    }

    // Monitor configuration
    if (auto monitor_pt = pt.get_child_optional("monitor")) {
        monitor.enable_metrics =
            get_value(*monitor_pt, "enable_metrics", monitor.enable_metrics);
        monitor.enable_tracing =
            get_value(*monitor_pt, "enable_tracing", monitor.enable_tracing);
        monitor.metrics_interval = get_value(*monitor_pt, "metrics_interval",
                                             monitor.metrics_interval);
        monitor.metrics_output =
            get_value(*monitor_pt, "metrics_output", monitor.metrics_output);
    }

    // Memory configuration
    if (auto memory_pt = pt.get_child_optional("memory")) {
        memory.max_memory_per_actor = get_value(
            *memory_pt, "max_memory_per_actor", memory.max_memory_per_actor);
        memory.message_buffer_size = get_value(
            *memory_pt, "message_buffer_size", memory.message_buffer_size);
        memory.enable_gc = get_value(*memory_pt, "enable_gc", memory.enable_gc);
        memory.gc_interval =
            get_value(*memory_pt, "gc_interval", memory.gc_interval);
    }
}

YAML::Node ActorSystemConfig::to_yaml() const {
    YAML::Node node_yaml;

    // Node configuration
    node_yaml["node"]["node_id"] = node.node_id;
    node_yaml["node"]["cluster_name"] = node.cluster_name;
    node_yaml["node"]["auto_generate_node_id"] = node.auto_generate_node_id;

    // Scheduler configuration
    node_yaml["scheduler"]["policy"] = scheduler.policy;
    node_yaml["scheduler"]["worker_threads"] = scheduler.worker_threads;
    node_yaml["scheduler"]["max_throughput"] = scheduler.max_throughput;
    node_yaml["scheduler"]["enable_profiling"] = scheduler.enable_profiling;

    // Network configuration
    node_yaml["network"]["enabled"] = network.enabled;
    node_yaml["network"]["host"] = network.host;
    node_yaml["network"]["port"] = network.port;
    node_yaml["network"]["max_connections"] = network.max_connections;
    node_yaml["network"]["connection_timeout"] = network.connection_timeout;

    // Monitor configuration
    node_yaml["monitor"]["enable_metrics"] = monitor.enable_metrics;
    node_yaml["monitor"]["enable_tracing"] = monitor.enable_tracing;
    node_yaml["monitor"]["metrics_interval"] = monitor.metrics_interval;
    node_yaml["monitor"]["metrics_output"] = monitor.metrics_output;

    // Memory configuration
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

    // Generate unique node ID based on hostname and PID
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