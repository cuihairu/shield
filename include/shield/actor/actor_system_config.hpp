#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shield/config/module_config.hpp"

namespace shield::actor {

// Actor系统配置
class ActorSystemConfig : public config::ModuleConfig {
public:
    // 节点配置
    struct NodeConfig {
        std::string node_id = "shield-node-1";
        std::string cluster_name = "shield-cluster";
        bool auto_generate_node_id = true;  // 自动生成基于主机名和PID的节点ID
    };

    // 调度器配置
    struct SchedulerConfig {
        std::string policy = "sharing";  // sharing, stealing
        int worker_threads = 0;          // 0表示使用硬件并发数
        int max_throughput = 300;        // 每轮调度的最大消息数
        bool enable_profiling = false;
    };

    // 网络配置
    struct NetworkConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 0;  // 0表示自动分配端口
        int max_connections = 100;
        int connection_timeout = 10000;  // milliseconds
    };

    // 监控配置
    struct MonitorConfig {
        bool enable_metrics = true;
        bool enable_tracing = false;
        int metrics_interval = 5000;                // milliseconds
        std::string metrics_output = "prometheus";  // prometheus, json, console
    };

    // 内存管理配置
    struct MemoryConfig {
        size_t max_memory_per_actor = 67108864;  // 64MB
        size_t message_buffer_size = 1048576;    // 1MB
        bool enable_gc = true;
        int gc_interval = 30000;  // milliseconds
    };

    // 配置数据
    NodeConfig node;
    SchedulerConfig scheduler;
    NetworkConfig network;
    MonitorConfig monitor;
    MemoryConfig memory;

    // ModuleConfig接口实现
    void from_yaml(const YAML::Node& node) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string module_name() const override { return "actor_system"; }

    // 便利方法
    int get_effective_worker_threads() const;
    std::string get_effective_node_id() const;
    bool is_network_enabled() const { return network.enabled; }
};

}  // namespace shield::actor