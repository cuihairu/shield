#include "shield/gateway/gateway_config.hpp"

#include <stdexcept>
#include <thread>

#include "shield/log/logger.hpp"

namespace shield::gateway {

void GatewayConfig::from_yaml(const YAML::Node& node) {
    if (node["listener"]) {
        auto listener_node = node["listener"];
        if (listener_node["host"])
            listener.host = listener_node["host"].as<std::string>();
        if (listener_node["port"])
            listener.port = listener_node["port"].as<uint16_t>();
        if (listener_node["io_threads"])
            listener.io_threads = listener_node["io_threads"].as<int>();
    }

    if (node["tcp"]) {
        auto tcp_node = node["tcp"];
        if (tcp_node["enabled"]) tcp.enabled = tcp_node["enabled"].as<bool>();
        if (tcp_node["backlog"]) tcp.backlog = tcp_node["backlog"].as<int>();
        if (tcp_node["keep_alive"])
            tcp.keep_alive = tcp_node["keep_alive"].as<bool>();
        if (tcp_node["receive_buffer_size"])
            tcp.receive_buffer_size = tcp_node["receive_buffer_size"].as<int>();
        if (tcp_node["send_buffer_size"])
            tcp.send_buffer_size = tcp_node["send_buffer_size"].as<int>();
    }

    if (node["udp"]) {
        auto udp_node = node["udp"];
        if (udp_node["enabled"]) udp.enabled = udp_node["enabled"].as<bool>();
        if (udp_node["buffer_size"])
            udp.buffer_size = udp_node["buffer_size"].as<int>();
        if (udp_node["port"]) udp.port = udp_node["port"].as<uint16_t>();
    }

    if (node["http"]) {
        auto http_node = node["http"];
        if (http_node["enabled"])
            http.enabled = http_node["enabled"].as<bool>();
        if (http_node["port"]) http.port = http_node["port"].as<uint16_t>();
        if (http_node["root_path"])
            http.root_path = http_node["root_path"].as<std::string>();
        if (http_node["max_request_size"])
            http.max_request_size = http_node["max_request_size"].as<int>();
    }

    if (node["websocket"]) {
        auto ws_node = node["websocket"];
        if (ws_node["enabled"])
            websocket.enabled = ws_node["enabled"].as<bool>();
        if (ws_node["port"]) websocket.port = ws_node["port"].as<uint16_t>();
        if (ws_node["path"]) websocket.path = ws_node["path"].as<std::string>();
        if (ws_node["max_message_size"])
            websocket.max_message_size = ws_node["max_message_size"].as<int>();
        if (ws_node["ping_interval"])
            websocket.ping_interval = ws_node["ping_interval"].as<int>();
    }

    if (node["threading"]) {
        auto threading_node = node["threading"];
        if (threading_node["io_threads"])
            threading.io_threads = threading_node["io_threads"].as<int>();
        if (threading_node["worker_threads"])
            threading.worker_threads =
                threading_node["worker_threads"].as<int>();
    }
}

YAML::Node GatewayConfig::to_yaml() const {
    YAML::Node node;

    // Listener配置
    node["listener"]["host"] = listener.host;
    node["listener"]["port"] = listener.port;
    node["listener"]["io_threads"] = listener.io_threads;

    // TCP配置
    node["tcp"]["enabled"] = tcp.enabled;
    node["tcp"]["backlog"] = tcp.backlog;
    node["tcp"]["keep_alive"] = tcp.keep_alive;
    node["tcp"]["receive_buffer_size"] = tcp.receive_buffer_size;
    node["tcp"]["send_buffer_size"] = tcp.send_buffer_size;

    // UDP配置
    node["udp"]["enabled"] = udp.enabled;
    node["udp"]["buffer_size"] = udp.buffer_size;
    node["udp"]["port"] = udp.port;

    // HTTP配置
    node["http"]["enabled"] = http.enabled;
    node["http"]["port"] = http.port;
    node["http"]["root_path"] = http.root_path;
    node["http"]["max_request_size"] = http.max_request_size;

    // WebSocket配置
    node["websocket"]["enabled"] = websocket.enabled;
    node["websocket"]["port"] = websocket.port;
    node["websocket"]["path"] = websocket.path;
    node["websocket"]["max_message_size"] = websocket.max_message_size;
    node["websocket"]["ping_interval"] = websocket.ping_interval;

    // 线程配置
    node["threading"]["io_threads"] = threading.io_threads;
    node["threading"]["worker_threads"] = threading.worker_threads;

    return node;
}

void GatewayConfig::validate() const {
    if (listener.host.empty()) {
        throw std::invalid_argument("Gateway listener host cannot be empty");
    }

    if (listener.port == 0) {
        throw std::invalid_argument(
            "Gateway listener port must be greater than 0");
    }

    if (tcp.enabled && tcp.backlog <= 0) {
        throw std::invalid_argument("TCP backlog must be greater than 0");
    }

    if (udp.enabled && udp.buffer_size <= 0) {
        throw std::invalid_argument("UDP buffer size must be greater than 0");
    }

    if (http.enabled && http.port == 0) {
        throw std::invalid_argument(
            "HTTP port must be greater than 0 when HTTP is enabled");
    }

    if (websocket.enabled && websocket.ping_interval <= 0) {
        throw std::invalid_argument(
            "WebSocket ping interval must be greater than 0");
    }

    // 检查端口冲突
    if (http.enabled && http.port == listener.port) {
        throw std::invalid_argument(
            "HTTP port cannot be the same as listener port");
    }

    if (udp.enabled && udp.port == listener.port) {
        throw std::invalid_argument(
            "UDP port cannot be the same as listener port");
    }
}

int GatewayConfig::get_effective_io_threads() const {
    int threads =
        (threading.io_threads > 0) ? threading.io_threads : listener.io_threads;
    return (threads > 0)
               ? threads
               : static_cast<int>(std::thread::hardware_concurrency());
}

int GatewayConfig::get_effective_worker_threads() const {
    return (threading.worker_threads > 0)
               ? threading.worker_threads
               : static_cast<int>(std::thread::hardware_concurrency());
}

}  // namespace shield::gateway