#pragma once

#include <cstdint>
#include <string>

#include "shield/config/module_config.hpp"

namespace shield::net {

// TCP服务器配置
class TcpConfig : public shield::config::ModuleConfig {
public:
    // TCP基础配置
    struct ServerConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 8080;
        int backlog = 128;
        bool keep_alive = true;
        int max_connections = 1000;
    };

    // TCP缓冲区配置
    struct BufferConfig {
        int receive_buffer_size = 65536;
        int send_buffer_size = 65536;
        bool no_delay = true;          // TCP_NODELAY
        int keep_alive_idle = 7200;    // seconds
        int keep_alive_interval = 75;  // seconds
        int keep_alive_count = 9;
    };

    // TCP线程配置
    struct ThreadingConfig {
        int io_threads = 0;      // 0表示使用硬件并发数
        int worker_threads = 0;  // 0表示使用硬件并发数
        bool use_thread_pool = true;
    };

    // 配置数据
    ServerConfig server;
    BufferConfig buffer;
    ThreadingConfig threading;

    // ModuleConfig接口实现
    void from_yaml(const YAML::Node& node) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string module_name() const override { return "tcp"; }

    // 便利方法
    int get_effective_io_threads() const;
    int get_effective_worker_threads() const;
    bool is_enabled() const { return server.enabled; }
};

// UDP服务器配置
class UdpConfig : public config::ModuleConfig {
public:
    // UDP基础配置
    struct ServerConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 8081;
        int buffer_size = 65536;
        int max_packet_size = 1500;  // MTU size
    };

    // UDP性能配置
    struct PerformanceConfig {
        bool reuse_address = true;
        bool reuse_port = false;
        int receive_timeout = 5000;  // milliseconds
        int send_timeout = 5000;     // milliseconds
        int max_concurrent_packets = 1000;
    };

    // UDP线程配置
    struct ThreadingConfig {
        int io_threads = 0;      // 0表示使用硬件并发数
        int worker_threads = 0;  // 0表示使用硬件并发数
        bool use_thread_pool = true;
    };

    // 配置数据
    ServerConfig server;
    PerformanceConfig performance;
    ThreadingConfig threading;

    // ModuleConfig接口实现
    void from_yaml(const YAML::Node& node) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string module_name() const override { return "udp"; }

    // 便利方法
    int get_effective_io_threads() const;
    int get_effective_worker_threads() const;
    bool is_enabled() const { return server.enabled; }
};

}  // namespace shield::net