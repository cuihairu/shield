#pragma once

#include <cstdint>
#include <string>

#include "shield/config/module_config.hpp"

namespace shield::gateway {

// Gateway模块配置
class GatewayConfig : public config::ModuleConfig {
public:
    // 监听器配置
    struct ListenerConfig {
        std::string host = "0.0.0.0";
        uint16_t port = 8080;
        int io_threads = 0;  // 0表示使用硬件并发数
    };

    // TCP配置
    struct TcpConfig {
        bool enabled = true;
        int backlog = 128;
        bool keep_alive = true;
        int receive_buffer_size = 65536;
        int send_buffer_size = 65536;
    };

    // UDP配置
    struct UdpConfig {
        bool enabled = true;
        int buffer_size = 65536;
        uint16_t port = 8081;
    };

    // HTTP配置
    struct HttpConfig {
        bool enabled = false;
        uint16_t port = 8082;
        std::string root_path = "/";
        int max_request_size = 1048576;  // 1MB
    };

    // WebSocket配置
    struct WebSocketConfig {
        bool enabled = true;
        uint16_t port = 8083;
        std::string path = "/ws";
        int max_message_size = 1048576;  // 1MB
        int ping_interval = 30;          // seconds
    };

    // 线程配置
    struct ThreadingConfig {
        int io_threads = 0;      // 0表示使用硬件并发数
        int worker_threads = 0;  // 0表示使用硬件并发数
    };

    // 配置数据
    ListenerConfig listener;
    TcpConfig tcp;
    UdpConfig udp;
    HttpConfig http;
    WebSocketConfig websocket;
    ThreadingConfig threading;

    // ModuleConfig接口实现
    void from_yaml(const YAML::Node& node) override;
    YAML::Node to_yaml() const override;
    void validate() const override;
    std::string module_name() const override { return "gateway"; }

    // 便利方法
    int get_effective_io_threads() const;
    int get_effective_worker_threads() const;
};

}  // namespace shield::gateway