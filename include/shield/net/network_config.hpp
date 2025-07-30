#pragma once

#include <cstdint>
#include <string>

#include "shield/config/config.hpp"

namespace shield::net {

// TCP server configuration
class TcpConfig : public shield::config::ComponentConfig {
public:
    // TCP basic configuration
    struct ServerConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 8080;
        int backlog = 128;
        bool keep_alive = true;
        int max_connections = 1000;
    };

    // TCP buffer configuration
    struct BufferConfig {
        int receive_buffer_size = 65536;
        int send_buffer_size = 65536;
        bool no_delay = true;          // TCP_NODELAY
        int keep_alive_idle = 7200;    // seconds
        int keep_alive_interval = 75;  // seconds
        int keep_alive_count = 9;
    };

    // TCP threading configuration
    struct ThreadingConfig {
        int io_threads = 0;      // 0 means use hardware concurrency
        int worker_threads = 0;  // 0 means use hardware concurrency
        bool use_thread_pool = true;
    };

    // Configuration data
    ServerConfig server;
    BufferConfig buffer;
    ThreadingConfig threading;

    // ComponentConfig interface implementation
    void from_ptree(const boost::property_tree::ptree& pt) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string component_name() const override { return "tcp"; }
    CLONE_IMPL(TcpConfig)

    // Convenience methods
    int get_effective_io_threads() const;
    int get_effective_worker_threads() const;
    bool is_enabled() const { return server.enabled; }
};

// UDP server configuration
class UdpConfig : public config::ComponentConfig {
public:
    // UDP basic configuration
    struct ServerConfig {
        bool enabled = true;
        std::string host = "0.0.0.0";
        uint16_t port = 8081;
        int buffer_size = 65536;
        int max_packet_size = 1500;  // MTU size
    };

    // UDP performance configuration
    struct PerformanceConfig {
        bool reuse_address = true;
        bool reuse_port = false;
        int receive_timeout = 5000;  // milliseconds
        int send_timeout = 5000;     // milliseconds
        int max_concurrent_packets = 1000;
    };

    // UDP threading configuration
    struct ThreadingConfig {
        int io_threads = 0;      // 0 means use hardware concurrency
        int worker_threads = 0;  // 0 means use hardware concurrency
        bool use_thread_pool = true;
    };

    // Configuration data
    ServerConfig server;
    PerformanceConfig performance;
    ThreadingConfig threading;

    // ComponentConfig interface implementation
    void from_ptree(const boost::property_tree::ptree& pt) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string component_name() const override { return "udp"; }
    CLONE_IMPL(UdpConfig)

    // Convenience methods
    int get_effective_io_threads() const;
    int get_effective_worker_threads() const;
    bool is_enabled() const { return server.enabled; }
};

}  // namespace shield::net