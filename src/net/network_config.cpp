#include "shield/net/network_config.hpp"

#include <stdexcept>
#include <thread>

#include "shield/log/logger.hpp"

namespace shield::net {

// TcpConfig implementation
void TcpConfig::from_yaml(const YAML::Node& node) {
    if (node["server"]) {
        auto server_node = node["server"];
        if (server_node["enabled"])
            server.enabled = server_node["enabled"].as<bool>();
        if (server_node["host"])
            server.host = server_node["host"].as<std::string>();
        if (server_node["port"])
            server.port = server_node["port"].as<uint16_t>();
        if (server_node["backlog"])
            server.backlog = server_node["backlog"].as<int>();
        if (server_node["keep_alive"])
            server.keep_alive = server_node["keep_alive"].as<bool>();
        if (server_node["max_connections"])
            server.max_connections = server_node["max_connections"].as<int>();
    }

    if (node["buffer"]) {
        auto buffer_node = node["buffer"];
        if (buffer_node["receive_buffer_size"])
            buffer.receive_buffer_size =
                buffer_node["receive_buffer_size"].as<int>();
        if (buffer_node["send_buffer_size"])
            buffer.send_buffer_size = buffer_node["send_buffer_size"].as<int>();
        if (buffer_node["no_delay"])
            buffer.no_delay = buffer_node["no_delay"].as<bool>();
        if (buffer_node["keep_alive_idle"])
            buffer.keep_alive_idle = buffer_node["keep_alive_idle"].as<int>();
        if (buffer_node["keep_alive_interval"])
            buffer.keep_alive_interval =
                buffer_node["keep_alive_interval"].as<int>();
        if (buffer_node["keep_alive_count"])
            buffer.keep_alive_count = buffer_node["keep_alive_count"].as<int>();
    }

    if (node["threading"]) {
        auto threading_node = node["threading"];
        if (threading_node["io_threads"])
            threading.io_threads = threading_node["io_threads"].as<int>();
        if (threading_node["worker_threads"])
            threading.worker_threads =
                threading_node["worker_threads"].as<int>();
        if (threading_node["use_thread_pool"])
            threading.use_thread_pool =
                threading_node["use_thread_pool"].as<bool>();
    }
}

YAML::Node TcpConfig::to_yaml() const {
    YAML::Node node;

    // Server配置
    node["server"]["enabled"] = server.enabled;
    node["server"]["host"] = server.host;
    node["server"]["port"] = server.port;
    node["server"]["backlog"] = server.backlog;
    node["server"]["keep_alive"] = server.keep_alive;
    node["server"]["max_connections"] = server.max_connections;

    // Buffer配置
    node["buffer"]["receive_buffer_size"] = buffer.receive_buffer_size;
    node["buffer"]["send_buffer_size"] = buffer.send_buffer_size;
    node["buffer"]["no_delay"] = buffer.no_delay;
    node["buffer"]["keep_alive_idle"] = buffer.keep_alive_idle;
    node["buffer"]["keep_alive_interval"] = buffer.keep_alive_interval;
    node["buffer"]["keep_alive_count"] = buffer.keep_alive_count;

    // Threading配置
    node["threading"]["io_threads"] = threading.io_threads;
    node["threading"]["worker_threads"] = threading.worker_threads;
    node["threading"]["use_thread_pool"] = threading.use_thread_pool;

    return node;
}

void TcpConfig::validate() const {
    if (server.enabled) {
        if (server.host.empty()) {
            throw std::invalid_argument(
                "TCP server host cannot be empty when enabled");
        }

        if (server.port == 0) {
            throw std::invalid_argument(
                "TCP server port must be greater than 0");
        }

        if (server.backlog <= 0) {
            throw std::invalid_argument("TCP backlog must be greater than 0");
        }

        if (server.max_connections <= 0) {
            throw std::invalid_argument(
                "TCP max connections must be greater than 0");
        }
    }

    if (buffer.receive_buffer_size <= 0) {
        throw std::invalid_argument(
            "TCP receive buffer size must be greater than 0");
    }

    if (buffer.send_buffer_size <= 0) {
        throw std::invalid_argument(
            "TCP send buffer size must be greater than 0");
    }
}

int TcpConfig::get_effective_io_threads() const {
    return (threading.io_threads > 0)
               ? threading.io_threads
               : static_cast<int>(std::thread::hardware_concurrency());
}

int TcpConfig::get_effective_worker_threads() const {
    return (threading.worker_threads > 0)
               ? threading.worker_threads
               : static_cast<int>(std::thread::hardware_concurrency());
}

// UdpConfig implementation
void UdpConfig::from_yaml(const YAML::Node& node) {
    if (node["server"]) {
        auto server_node = node["server"];
        if (server_node["enabled"])
            server.enabled = server_node["enabled"].as<bool>();
        if (server_node["host"])
            server.host = server_node["host"].as<std::string>();
        if (server_node["port"])
            server.port = server_node["port"].as<uint16_t>();
        if (server_node["buffer_size"])
            server.buffer_size = server_node["buffer_size"].as<int>();
        if (server_node["max_packet_size"])
            server.max_packet_size = server_node["max_packet_size"].as<int>();
    }

    if (node["performance"]) {
        auto perf_node = node["performance"];
        if (perf_node["reuse_address"])
            performance.reuse_address = perf_node["reuse_address"].as<bool>();
        if (perf_node["reuse_port"])
            performance.reuse_port = perf_node["reuse_port"].as<bool>();
        if (perf_node["receive_timeout"])
            performance.receive_timeout =
                perf_node["receive_timeout"].as<int>();
        if (perf_node["send_timeout"])
            performance.send_timeout = perf_node["send_timeout"].as<int>();
        if (perf_node["max_concurrent_packets"])
            performance.max_concurrent_packets =
                perf_node["max_concurrent_packets"].as<int>();
    }

    if (node["threading"]) {
        auto threading_node = node["threading"];
        if (threading_node["io_threads"])
            threading.io_threads = threading_node["io_threads"].as<int>();
        if (threading_node["worker_threads"])
            threading.worker_threads =
                threading_node["worker_threads"].as<int>();
        if (threading_node["use_thread_pool"])
            threading.use_thread_pool =
                threading_node["use_thread_pool"].as<bool>();
    }
}

YAML::Node UdpConfig::to_yaml() const {
    YAML::Node node;

    // Server配置
    node["server"]["enabled"] = server.enabled;
    node["server"]["host"] = server.host;
    node["server"]["port"] = server.port;
    node["server"]["buffer_size"] = server.buffer_size;
    node["server"]["max_packet_size"] = server.max_packet_size;

    // Performance配置
    node["performance"]["reuse_address"] = performance.reuse_address;
    node["performance"]["reuse_port"] = performance.reuse_port;
    node["performance"]["receive_timeout"] = performance.receive_timeout;
    node["performance"]["send_timeout"] = performance.send_timeout;
    node["performance"]["max_concurrent_packets"] =
        performance.max_concurrent_packets;

    // Threading配置
    node["threading"]["io_threads"] = threading.io_threads;
    node["threading"]["worker_threads"] = threading.worker_threads;
    node["threading"]["use_thread_pool"] = threading.use_thread_pool;

    return node;
}

void UdpConfig::validate() const {
    if (server.enabled) {
        if (server.host.empty()) {
            throw std::invalid_argument(
                "UDP server host cannot be empty when enabled");
        }

        if (server.port == 0) {
            throw std::invalid_argument(
                "UDP server port must be greater than 0");
        }

        if (server.buffer_size <= 0) {
            throw std::invalid_argument(
                "UDP buffer size must be greater than 0");
        }

        if (server.max_packet_size <= 0 || server.max_packet_size > 65507) {
            throw std::invalid_argument(
                "UDP max packet size must be between 1 and 65507 bytes");
        }
    }

    if (performance.receive_timeout <= 0) {
        throw std::invalid_argument(
            "UDP receive timeout must be greater than 0");
    }

    if (performance.send_timeout <= 0) {
        throw std::invalid_argument("UDP send timeout must be greater than 0");
    }
}

int UdpConfig::get_effective_io_threads() const {
    return (threading.io_threads > 0)
               ? threading.io_threads
               : static_cast<int>(std::thread::hardware_concurrency());
}

int UdpConfig::get_effective_worker_threads() const {
    return (threading.worker_threads > 0)
               ? threading.worker_threads
               : static_cast<int>(std::thread::hardware_concurrency());
}

}  // namespace shield::net