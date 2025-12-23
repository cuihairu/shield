#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "shield/config/config.hpp"

namespace shield::discovery {

class DiscoveryConfig
    : public config::ReloadableConfigurationProperties<DiscoveryConfig> {
public:
    struct LocalConfig {
        int cleanup_interval_seconds = 300;
        std::string persistence_file_path;
    };

    struct EtcdConfig {
        std::vector<std::string> endpoints;
    };

    struct ConsulConfig {
        std::string host = "127.0.0.1";
        int port = 8500;
        int check_interval_seconds = 10;
    };

    struct NacosConfig {
        std::string server_addr = "127.0.0.1:8848";
        int heartbeat_interval_seconds = 5;
    };

    struct RedisConfig {
        std::string host = "127.0.0.1";
        int port = 6379;
        std::string password;
        int db = 0;
        int heartbeat_interval_seconds = 5;
    };

    std::string type = "local";  // local/etcd/consul/nacos/redis
    LocalConfig local;
    EtcdConfig etcd;
    ConsulConfig consul;
    NacosConfig nacos;
    RedisConfig redis;

    void from_ptree(const boost::property_tree::ptree& pt) override;
    void validate() const override;
    std::string properties_name() const override { return "discovery"; }

    std::string build_redis_uri() const;
    std::chrono::seconds redis_heartbeat_interval() const;
};

}  // namespace shield::discovery
