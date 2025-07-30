#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shield::core {

namespace config {

struct TcpConfig {
    bool enabled = true;
    int backlog = 128;
    bool keep_alive = true;
};

struct UdpConfig {
    bool enabled = true;
    int buffer_size = 65536;
};

struct WebsocketConfig {
    bool enabled = true;
    std::string path = "/ws";
};

struct NetworkConfig {
    TcpConfig tcp;
    UdpConfig udp;
    WebsocketConfig websocket;
};

struct ActorSystemConfig {
    std::string node_id = "shield-node-1";
    int worker_threads = 4;
    std::string scheduler_policy = "sharing";
};

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    int max_connections = 1000;
    ActorSystemConfig actor_system;
    NetworkConfig network;
};

struct LoggerConfig {
    std::string level = "info";
    bool console_output = true;
    std::string log_file = "logs/shield.log";
    int64_t max_file_size = 10485760;  // 10MB
    int max_files = 5;
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%f] [%t] [%l] %v";
};

struct EtcdConfig {
    std::vector<std::string> endpoints = {"127.0.0.1:2379"};
};

struct ConsulConfig {
    std::string host = "127.0.0.1";
    int port = 8500;
};

struct NacosConfig {
    std::string server_addr = "127.0.0.1:8848";
};

struct RedisDiscoveryConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
};

struct DiscoveryConfig {
    std::string type = "local";
    EtcdConfig etcd;
    ConsulConfig consul;
    NacosConfig nacos;
    RedisDiscoveryConfig redis;
};

struct LuaConfig {
    bool enabled = true;
    std::string script_path = "scripts";
    int vm_pool_size = 8;
    int64_t max_memory = 67108864;  // 64MB
};

struct PrometheusConfig {
    bool enabled = true;
    int port = 9090;
    std::string path = "/metrics";
};

struct MetricsConfig {
    bool enabled = true;
    PrometheusConfig prometheus;
};

struct GameConfig {
    std::string name = "Shield Game";
    std::string version = "1.0.0";
    int max_players = 1000;
    int tick_rate = 60;
};

struct MysqlConfig {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string database = "shield_game";
    std::string username = "shield";
    std::string password = "password";
};

struct PostgresqlConfig {
    std::string host = "127.0.0.1";
    int port = 5432;
    std::string database = "shield_game";
    std::string username = "shield";
    std::string password = "password";
};

struct DatabaseConfig {
    std::string type = "none";
    MysqlConfig mysql;
    PostgresqlConfig postgresql;
};

// Main configuration struct
struct ShieldConfig {
    ServerConfig server;
    LoggerConfig logger;
    DiscoveryConfig discovery;
    LuaConfig lua;
    MetricsConfig metrics;
    GameConfig game;
    DatabaseConfig database;
};

}  // namespace config

// Function to get default configuration
config::ShieldConfig get_default_shield_config();

}  // namespace shield::core
