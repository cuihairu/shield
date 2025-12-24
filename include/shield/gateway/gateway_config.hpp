#pragma once

#include <cstdint>
#include <string>

#include "shield/config/config.hpp"

namespace shield::gateway {

// Gateway module configuration
class GatewayConfig
    : public config::ReloadableConfigurationProperties<GatewayConfig> {
public:
    // Listener configuration
    struct ListenerConfig {
        std::string host = "0.0.0.0";
        uint16_t port = 8080;
        int io_threads = 0;  // 0 means use hardware concurrency
    };

    // TCP configuration
    struct TcpConfig {
        bool enabled = true;
        int backlog = 128;
        bool keep_alive = true;
        int receive_buffer_size = 65536;
        int send_buffer_size = 65536;
    };

    // UDP configuration
    struct UdpConfig {
        bool enabled = true;
        int buffer_size = 65536;
        uint16_t port = 8081;
    };

    // HTTP configuration
    struct HttpConfig {
        bool enabled = false;
        uint16_t port = 8082;
        std::string root_path = "/";
        int max_request_size = 1048576;  // 1MB
        std::string backend = "beast";   // beast | legacy
    };

    // WebSocket configuration
    struct WebSocketConfig {
        bool enabled = true;
        uint16_t port = 8083;
        std::string path = "/ws";
        int max_message_size = 1048576;  // 1MB
        int ping_interval = 30;          // seconds
    };

    // Threading configuration
    struct ThreadingConfig {
        int io_threads = 0;      // 0 means use hardware concurrency
        int worker_threads = 0;  // 0 means use hardware concurrency
    };

    // Configuration data
    ListenerConfig listener;
    TcpConfig tcp;
    UdpConfig udp;
    HttpConfig http;
    WebSocketConfig websocket;
    ThreadingConfig threading;

    // ComponentConfig interface implementation
    void from_ptree(const boost::property_tree::ptree& pt) override;
    void validate() const override;
    std::string properties_name() const override { return "gateway"; }

    // Convenience methods
    int get_effective_io_threads() const;
    int get_effective_worker_threads() const;
};

}  // namespace shield::gateway
