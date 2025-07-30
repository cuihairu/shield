#include "shield/net/network_config.hpp"

#include <stdexcept>
#include <thread>

#include "shield/log/logger.hpp"

namespace shield::net {

// TcpConfig implementation
void TcpConfig::from_ptree(const boost::property_tree::ptree& pt) {
    // Server configuration
    if (auto server_pt = pt.get_child_optional("server")) {
        server.enabled = get_value(*server_pt, "enabled", server.enabled);
        server.host = get_value(*server_pt, "host", server.host);
        server.port = get_value(*server_pt, "port", server.port);
        server.backlog = get_value(*server_pt, "backlog", server.backlog);
        server.keep_alive =
            get_value(*server_pt, "keep_alive", server.keep_alive);
        server.max_connections =
            get_value(*server_pt, "max_connections", server.max_connections);
    }

    // Buffer configuration
    if (auto buffer_pt = pt.get_child_optional("buffer")) {
        buffer.receive_buffer_size = get_value(
            *buffer_pt, "receive_buffer_size", buffer.receive_buffer_size);
        buffer.send_buffer_size =
            get_value(*buffer_pt, "send_buffer_size", buffer.send_buffer_size);
        buffer.no_delay = get_value(*buffer_pt, "no_delay", buffer.no_delay);
        buffer.keep_alive_idle =
            get_value(*buffer_pt, "keep_alive_idle", buffer.keep_alive_idle);
        buffer.keep_alive_interval = get_value(
            *buffer_pt, "keep_alive_interval", buffer.keep_alive_interval);
        buffer.keep_alive_count =
            get_value(*buffer_pt, "keep_alive_count", buffer.keep_alive_count);
    }

    // Threading configuration
    if (auto threading_pt = pt.get_child_optional("threading")) {
        threading.io_threads =
            get_value(*threading_pt, "io_threads", threading.io_threads);
        threading.worker_threads = get_value(*threading_pt, "worker_threads",
                                             threading.worker_threads);
        threading.use_thread_pool = get_value(*threading_pt, "use_thread_pool",
                                              threading.use_thread_pool);
    }
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
void UdpConfig::from_ptree(const boost::property_tree::ptree& pt) {
    // Server configuration
    if (auto server_pt = pt.get_child_optional("server")) {
        server.enabled = get_value(*server_pt, "enabled", server.enabled);
        server.host = get_value(*server_pt, "host", server.host);
        server.port = get_value(*server_pt, "port", server.port);
        server.buffer_size =
            get_value(*server_pt, "buffer_size", server.buffer_size);
        server.max_packet_size =
            get_value(*server_pt, "max_packet_size", server.max_packet_size);
    }

    // Performance configuration
    if (auto perf_pt = pt.get_child_optional("performance")) {
        performance.reuse_address =
            get_value(*perf_pt, "reuse_address", performance.reuse_address);
        performance.reuse_port =
            get_value(*perf_pt, "reuse_port", performance.reuse_port);
        performance.receive_timeout =
            get_value(*perf_pt, "receive_timeout", performance.receive_timeout);
        performance.send_timeout =
            get_value(*perf_pt, "send_timeout", performance.send_timeout);
        performance.max_concurrent_packets =
            get_value(*perf_pt, "max_concurrent_packets",
                      performance.max_concurrent_packets);
    }

    // Threading configuration
    if (auto threading_pt = pt.get_child_optional("threading")) {
        threading.io_threads =
            get_value(*threading_pt, "io_threads", threading.io_threads);
        threading.worker_threads = get_value(*threading_pt, "worker_threads",
                                             threading.worker_threads);
        threading.use_thread_pool = get_value(*threading_pt, "use_thread_pool",
                                              threading.use_thread_pool);
    }
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