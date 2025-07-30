#include "shield/gateway/gateway_config.hpp"

#include <stdexcept>
#include <thread>

#include "shield/log/logger.hpp"

namespace shield::gateway {

void GatewayConfig::from_ptree(const boost::property_tree::ptree& pt) {
    // Listener configuration
    if (auto listener_pt = pt.get_child_optional("listener")) {
        listener.host = get_value(*listener_pt, "host", listener.host);
        listener.port = get_value(*listener_pt, "port", listener.port);
        listener.io_threads =
            get_value(*listener_pt, "io_threads", listener.io_threads);
    }

    // TCP configuration
    if (auto tcp_pt = pt.get_child_optional("tcp")) {
        tcp.enabled = get_value(*tcp_pt, "enabled", tcp.enabled);
        tcp.backlog = get_value(*tcp_pt, "backlog", tcp.backlog);
        tcp.keep_alive = get_value(*tcp_pt, "keep_alive", tcp.keep_alive);
        tcp.receive_buffer_size =
            get_value(*tcp_pt, "receive_buffer_size", tcp.receive_buffer_size);
        tcp.send_buffer_size =
            get_value(*tcp_pt, "send_buffer_size", tcp.send_buffer_size);
    }

    // UDP configuration
    if (auto udp_pt = pt.get_child_optional("udp")) {
        udp.enabled = get_value(*udp_pt, "enabled", udp.enabled);
        udp.buffer_size = get_value(*udp_pt, "buffer_size", udp.buffer_size);
        udp.port = get_value(*udp_pt, "port", udp.port);
    }

    // HTTP configuration
    if (auto http_pt = pt.get_child_optional("http")) {
        http.enabled = get_value(*http_pt, "enabled", http.enabled);
        http.port = get_value(*http_pt, "port", http.port);
        http.root_path = get_value(*http_pt, "root_path", http.root_path);
        http.max_request_size =
            get_value(*http_pt, "max_request_size", http.max_request_size);
    }

    // WebSocket configuration
    if (auto ws_pt = pt.get_child_optional("websocket")) {
        websocket.enabled = get_value(*ws_pt, "enabled", websocket.enabled);
        websocket.port = get_value(*ws_pt, "port", websocket.port);
        websocket.path = get_value(*ws_pt, "path", websocket.path);
        websocket.max_message_size =
            get_value(*ws_pt, "max_message_size", websocket.max_message_size);
        websocket.ping_interval =
            get_value(*ws_pt, "ping_interval", websocket.ping_interval);
    }

    // Threading configuration
    if (auto threading_pt = pt.get_child_optional("threading")) {
        threading.io_threads =
            get_value(*threading_pt, "io_threads", threading.io_threads);
        threading.worker_threads = get_value(*threading_pt, "worker_threads",
                                             threading.worker_threads);
    }
}

YAML::Node GatewayConfig::to_yaml() const {
    YAML::Node node;

    // Listener configuration
    node["listener"]["host"] = listener.host;
    node["listener"]["port"] = listener.port;
    node["listener"]["io_threads"] = listener.io_threads;

    // TCP configuration
    node["tcp"]["enabled"] = tcp.enabled;
    node["tcp"]["backlog"] = tcp.backlog;
    node["tcp"]["keep_alive"] = tcp.keep_alive;
    node["tcp"]["receive_buffer_size"] = tcp.receive_buffer_size;
    node["tcp"]["send_buffer_size"] = tcp.send_buffer_size;

    // UDP configuration
    node["udp"]["enabled"] = udp.enabled;
    node["udp"]["buffer_size"] = udp.buffer_size;
    node["udp"]["port"] = udp.port;

    // HTTP configuration
    node["http"]["enabled"] = http.enabled;
    node["http"]["port"] = http.port;
    node["http"]["root_path"] = http.root_path;
    node["http"]["max_request_size"] = http.max_request_size;

    // WebSocket configuration
    node["websocket"]["enabled"] = websocket.enabled;
    node["websocket"]["port"] = websocket.port;
    node["websocket"]["path"] = websocket.path;
    node["websocket"]["max_message_size"] = websocket.max_message_size;
    node["websocket"]["ping_interval"] = websocket.ping_interval;

    // Threading configuration
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

    // Check port conflicts
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