#include "shield/discovery/discovery_config.hpp"

#include <stdexcept>

namespace shield::discovery {

void DiscoveryConfig::from_ptree(const boost::property_tree::ptree& pt) {
    type = get_value(pt, "type", type);

    if (auto local_pt = pt.get_child_optional("local")) {
        local.cleanup_interval_seconds =
            get_value(*local_pt, "cleanup_interval_seconds",
                      local.cleanup_interval_seconds);
        local.persistence_file_path = get_value(
            *local_pt, "persistence_file_path", local.persistence_file_path);
    }

    if (auto etcd_pt = pt.get_child_optional("etcd")) {
        load_vector(*etcd_pt, "endpoints", etcd.endpoints);
    }

    if (auto consul_pt = pt.get_child_optional("consul")) {
        consul.host = get_value(*consul_pt, "host", consul.host);
        consul.port = get_value(*consul_pt, "port", consul.port);
        consul.check_interval_seconds =
            get_value(*consul_pt, "check_interval_seconds",
                      consul.check_interval_seconds);
    }

    if (auto nacos_pt = pt.get_child_optional("nacos")) {
        nacos.server_addr =
            get_value(*nacos_pt, "server_addr", nacos.server_addr);
        nacos.heartbeat_interval_seconds =
            get_value(*nacos_pt, "heartbeat_interval_seconds",
                      nacos.heartbeat_interval_seconds);
    }

    if (auto redis_pt = pt.get_child_optional("redis")) {
        redis.host = get_value(*redis_pt, "host", redis.host);
        redis.port = get_value(*redis_pt, "port", redis.port);
        redis.password = get_value(*redis_pt, "password", redis.password);
        redis.db = get_value(*redis_pt, "db", redis.db);
        redis.heartbeat_interval_seconds =
            get_value(*redis_pt, "heartbeat_interval_seconds",
                      redis.heartbeat_interval_seconds);
    }
}

void DiscoveryConfig::validate() const {
    if (type != "local" && type != "etcd" && type != "consul" &&
        type != "nacos" && type != "redis") {
        throw std::invalid_argument("Unsupported discovery.type: " + type);
    }

    if (type == "local") {
        if (local.cleanup_interval_seconds <= 0) {
            throw std::invalid_argument(
                "discovery.local.cleanup_interval_seconds must be > 0");
        }
    } else if (type == "etcd") {
        if (etcd.endpoints.empty()) {
            throw std::invalid_argument(
                "discovery.etcd.endpoints must not be empty when using etcd");
        }
    } else if (type == "consul") {
        if (consul.host.empty()) {
            throw std::invalid_argument(
                "discovery.consul.host must not be empty");
        }
        if (consul.port <= 0) {
            throw std::invalid_argument("discovery.consul.port must be > 0");
        }
        if (consul.check_interval_seconds <= 0) {
            throw std::invalid_argument(
                "discovery.consul.check_interval_seconds must be > 0");
        }
    } else if (type == "nacos") {
        if (nacos.server_addr.empty()) {
            throw std::invalid_argument(
                "discovery.nacos.server_addr must not be empty");
        }
        if (nacos.heartbeat_interval_seconds <= 0) {
            throw std::invalid_argument(
                "discovery.nacos.heartbeat_interval_seconds must be > 0");
        }
    } else if (type == "redis") {
        if (redis.host.empty()) {
            throw std::invalid_argument(
                "discovery.redis.host must not be empty");
        }
        if (redis.port <= 0) {
            throw std::invalid_argument("discovery.redis.port must be > 0");
        }
        if (redis.db < 0) {
            throw std::invalid_argument("discovery.redis.db must be >= 0");
        }
        if (redis.heartbeat_interval_seconds <= 0) {
            throw std::invalid_argument(
                "discovery.redis.heartbeat_interval_seconds must be > 0");
        }
    }
}

std::string DiscoveryConfig::build_redis_uri() const {
    std::string uri = "tcp://";
    if (!redis.password.empty()) {
        uri += ":" + redis.password + "@";
    }
    uri += redis.host + ":" + std::to_string(redis.port);
    if (redis.db != 0) {
        uri += "/" + std::to_string(redis.db);
    }
    return uri;
}

std::chrono::seconds DiscoveryConfig::redis_heartbeat_interval() const {
    return std::chrono::seconds(redis.heartbeat_interval_seconds);
}

}  // namespace shield::discovery
