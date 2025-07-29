#pragma once

#include <chrono>  // Added for std::chrono
#include <memory>
#include <string>

#include "shield/core/config.hpp"
#include "shield/core/logger.hpp"
#include "shield/discovery/consul_discovery.hpp"
#include "shield/discovery/etcd_discovery.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/discovery/nacos_discovery.hpp"
#include "shield/discovery/redis_discovery.hpp"
#include "shield/discovery/service_discovery.hpp"

namespace shield {
namespace discovery {

class ServiceDiscoveryFactory {
public:
    static std::unique_ptr<IServiceDiscovery> create_service_discovery(
        const core::Config &config) {
        std::string type = config.get_service_discovery_type();

        if (type == "consul") {
            SHIELD_LOG_INFO << "Creating ConsulDiscovery client.";
            return make_consul_discovery(
                config.get_consul_host() + ":" +
                    std::to_string(config.get_consul_port()),
                std::chrono::seconds(config.get_consul_check_interval_ms() /
                                     1000));  // Convert ms to s
        } else if (type == "etcd") {
            SHIELD_LOG_INFO << "Creating EtcdDiscovery client.";
            return make_etcd_discovery(config.get_etcd_endpoints()[0]);
        } else if (type == "nacos") {
            SHIELD_LOG_INFO << "Creating NacosDiscovery client.";
            return make_nacos_discovery(
                config.get_nacos_server_addresses()[0],
                std::chrono::seconds(10));  // Placeholder heartbeat interval
        } else if (type == "redis") {
            SHIELD_LOG_INFO << "Creating RedisDiscovery client.";
            std::string redis_uri = "redis://";
            if (!config.get_redis_password().empty()) {
                redis_uri += ":" + config.get_redis_password() + "@";
            }
            redis_uri += config.get_redis_host() + ":" +
                         std::to_string(config.get_redis_port());
            if (config.get_redis_db() != 0) {
                redis_uri += "/" + std::to_string(config.get_redis_db());
            }
            return make_redis_discovery(
                redis_uri,
                std::chrono::seconds(10));  // Default heartbeat interval
        } else if (type == "local") {
            SHIELD_LOG_INFO << "Creating LocalDiscovery client.";
            return make_local_discovery(
                std::chrono::seconds(300),
                config.get_local_discovery_file_path());  // Default cleanup
                                                          // interval
        } else {
            SHIELD_LOG_ERROR << "Unsupported discovery type: " << type
                             << ". Falling back to LocalDiscovery.";
            return make_local_discovery(std::chrono::seconds(300),
                                        config.get_local_discovery_file_path());
        }
    }
};

}  // namespace discovery
}  // namespace shield
