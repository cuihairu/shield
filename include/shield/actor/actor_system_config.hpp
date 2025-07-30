#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shield/config/config.hpp"

namespace shield::actor {

// Actor system configuration
class ActorSystemConfig
    : public config::ReloadableConfigurationProperties<ActorSystemConfig> {
public:
    // Node configuration
    struct NodeConfig {
        std::string node_id = "shield-node-1";
        std::string cluster_name = "shield-cluster";
        bool auto_generate_node_id =
            true;  // Auto-generate node ID based on hostname and PID
    };

    // Scheduler configuration
    struct SchedulerConfig {
        std::string policy = "sharing";  // sharing, stealing
        int worker_threads = 0;          // 0 means use hardware concurrency
        int max_throughput = 300;  // Maximum messages per scheduling round
        bool enable_profiling = false;
    };

    // Network configuration
    struct NetworkConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 0;  // 0 means auto-assign port
        int max_connections = 100;
        int connection_timeout = 10000;  // milliseconds
    };

    // Monitoring configuration
    struct MonitorConfig {
        bool enable_metrics = true;
        bool enable_tracing = false;
        int metrics_interval = 5000;                // milliseconds
        std::string metrics_output = "prometheus";  // prometheus, json, console
    };

    // Memory management configuration
    struct MemoryConfig {
        size_t max_memory_per_actor = 67108864;  // 64MB
        size_t message_buffer_size = 1048576;    // 1MB
        bool enable_gc = true;
        int gc_interval = 30000;  // milliseconds
    };

    // Configuration data
    NodeConfig node;
    SchedulerConfig scheduler;
    NetworkConfig network;
    MonitorConfig monitor;
    MemoryConfig memory;

    // ComponentConfig interface implementation
    void from_ptree(const boost::property_tree::ptree& pt) override;
    void validate() const override;
    std::string properties_name() const override { return "actor_system"; }

    // Convenience methods
    int get_effective_worker_threads() const;
    std::string get_effective_node_id() const;
};

}  // namespace shield::actor