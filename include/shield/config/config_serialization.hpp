#pragma once

#include <yaml-cpp/yaml.h>

#include "shield/config/config_def.hpp"

namespace YAML {

using namespace shield::config::config;

// Serialization for TcpConfig
template <>
struct convert<TcpConfig> {
    static Node encode(const TcpConfig& rhs) {
        Node node;
        node["enabled"] = rhs.enabled;
        node["backlog"] = rhs.backlog;
        node["keep_alive"] = rhs.keep_alive;
        return node;
    }
};

// Serialization for UdpConfig
template <>
struct convert<UdpConfig> {
    static Node encode(const UdpConfig& rhs) {
        Node node;
        node["enabled"] = rhs.enabled;
        node["buffer_size"] = rhs.buffer_size;
        return node;
    }
};

// Serialization for WebsocketConfig
template <>
struct convert<WebsocketConfig> {
    static Node encode(const WebsocketConfig& rhs) {
        Node node;
        node["enabled"] = rhs.enabled;
        node["path"] = rhs.path;
        return node;
    }
};

// Serialization for NetworkConfig
template <>
struct convert<NetworkConfig> {
    static Node encode(const NetworkConfig& rhs) {
        Node node;
        node["tcp"] = rhs.tcp;
        node["udp"] = rhs.udp;
        node["websocket"] = rhs.websocket;
        return node;
    }
};

// Serialization for ActorSystemConfig
template <>
struct convert<ActorSystemConfig> {
    static Node encode(const ActorSystemConfig& rhs) {
        Node node;
        node["node_id"] = rhs.node_id;
        node["worker_threads"] = rhs.worker_threads;
        node["scheduler_policy"] = rhs.scheduler_policy;
        return node;
    }
};

// Serialization for ServerConfig
template <>
struct convert<ServerConfig> {
    static Node encode(const ServerConfig& rhs) {
        Node node;
        node["host"] = rhs.host;
        node["port"] = rhs.port;
        node["max_connections"] = rhs.max_connections;
        node["actor_system"] = rhs.actor_system;
        node["network"] = rhs.network;
        return node;
    }
};

// Serialization for LoggerConfig
template <>
struct convert<LoggerConfig> {
    static Node encode(const LoggerConfig& rhs) {
        Node node;
        node["level"] = rhs.level;
        node["console_output"] = rhs.console_output;
        node["log_file"] = rhs.log_file;
        node["max_file_size"] = rhs.max_file_size;
        node["max_files"] = rhs.max_files;
        node["pattern"] = rhs.pattern;
        return node;
    }
};

// Serialization for EtcdConfig
template <>
struct convert<EtcdConfig> {
    static Node encode(const EtcdConfig& rhs) {
        Node node;
        node["endpoints"] = rhs.endpoints;
        return node;
    }
};

// Serialization for ConsulConfig
template <>
struct convert<ConsulConfig> {
    static Node encode(const ConsulConfig& rhs) {
        Node node;
        node["host"] = rhs.host;
        node["port"] = rhs.port;
        return node;
    }
};

// Serialization for NacosConfig
template <>
struct convert<NacosConfig> {
    static Node encode(const NacosConfig& rhs) {
        Node node;
        node["server_addr"] = rhs.server_addr;
        return node;
    }
};

// Serialization for RedisDiscoveryConfig
template <>
struct convert<RedisDiscoveryConfig> {
    static Node encode(const RedisDiscoveryConfig& rhs) {
        Node node;
        node["host"] = rhs.host;
        node["port"] = rhs.port;
        return node;
    }
};

// Serialization for DiscoveryConfig
template <>
struct convert<DiscoveryConfig> {
    static Node encode(const DiscoveryConfig& rhs) {
        Node node;
        node["type"] = rhs.type;
        node["etcd"] = rhs.etcd;
        node["consul"] = rhs.consul;
        node["nacos"] = rhs.nacos;
        node["redis"] = rhs.redis;
        return node;
    }
};

// Serialization for LuaConfig
template <>
struct convert<LuaConfig> {
    static Node encode(const LuaConfig& rhs) {
        Node node;
        node["enabled"] = rhs.enabled;
        node["script_path"] = rhs.script_path;
        node["vm_pool_size"] = rhs.vm_pool_size;
        node["max_memory"] = rhs.max_memory;
        return node;
    }
};

// Serialization for PrometheusConfig
template <>
struct convert<PrometheusConfig> {
    static Node encode(const PrometheusConfig& rhs) {
        Node node;
        node["enabled"] = rhs.enabled;
        node["port"] = rhs.port;
        node["path"] = rhs.path;
        return node;
    }
};

// Serialization for MetricsConfig
template <>
struct convert<MetricsConfig> {
    static Node encode(const MetricsConfig& rhs) {
        Node node;
        node["enabled"] = rhs.enabled;
        node["prometheus"] = rhs.prometheus;
        return node;
    }
};

// Serialization for GameConfig
template <>
struct convert<GameConfig> {
    static Node encode(const GameConfig& rhs) {
        Node node;
        node["name"] = rhs.name;
        node["version"] = rhs.version;
        node["max_players"] = rhs.max_players;
        node["tick_rate"] = rhs.tick_rate;
        return node;
    }
};

// Serialization for MysqlConfig
template <>
struct convert<MysqlConfig> {
    static Node encode(const MysqlConfig& rhs) {
        Node node;
        node["host"] = rhs.host;
        node["port"] = rhs.port;
        node["database"] = rhs.database;
        node["username"] = rhs.username;
        node["password"] = rhs.password;
        return node;
    }
};

// Serialization for PostgresqlConfig
template <>
struct convert<PostgresqlConfig> {
    static Node encode(const PostgresqlConfig& rhs) {
        Node node;
        node["host"] = rhs.host;
        node["port"] = rhs.port;
        node["database"] = rhs.database;
        node["username"] = rhs.username;
        node["password"] = rhs.password;
        return node;
    }
};

// Serialization for DatabaseConfig
template <>
struct convert<DatabaseConfig> {
    static Node encode(const DatabaseConfig& rhs) {
        Node node;
        node["type"] = rhs.type;
        node["mysql"] = rhs.mysql;
        node["postgresql"] = rhs.postgresql;
        return node;
    }
};

// Serialization for ShieldConfig
template <>
struct convert<ShieldConfig> {
    static Node encode(const ShieldConfig& rhs) {
        Node node;
        node["server"] = rhs.server;
        node["logger"] = rhs.logger;
        node["discovery"] = rhs.discovery;
        node["lua"] = rhs.lua;
        node["metrics"] = rhs.metrics;
        node["game"] = rhs.game;
        node["database"] = rhs.database;
        return node;
    }
};

}  // namespace YAML
