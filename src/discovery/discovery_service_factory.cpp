#include "shield/discovery/discovery_service_factory.hpp"

#include <chrono>

#include "shield/discovery/consul_discovery.hpp"
#include "shield/discovery/etcd_discovery.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/discovery/nacos_discovery.hpp"
#include "shield/discovery/redis_discovery.hpp"
#include "shield/log/logger.hpp"

namespace shield::discovery {

std::shared_ptr<IServiceDiscovery> create_discovery_service(
    const DiscoveryConfig* config) {
    if (!config) {
        auto instance = make_local_discovery();
        return std::shared_ptr<IServiceDiscovery>(std::move(instance));
    }

    if (config->type == "redis") {
        auto instance = make_redis_discovery(config->build_redis_uri(),
                                             config->redis_heartbeat_interval());
        return std::shared_ptr<IServiceDiscovery>(std::move(instance));
    }

    if (config->type == "nacos") {
        auto instance = make_nacos_discovery(
            config->nacos.server_addr,
            std::chrono::seconds(config->nacos.heartbeat_interval_seconds));
        return std::shared_ptr<IServiceDiscovery>(std::move(instance));
    }

    if (config->type == "consul") {
        auto instance = make_consul_discovery(
            config->consul.host + ":" + std::to_string(config->consul.port),
            std::chrono::seconds(config->consul.check_interval_seconds));
        return std::shared_ptr<IServiceDiscovery>(std::move(instance));
    }

    if (config->type == "etcd") {
        std::string endpoints;
        for (size_t i = 0; i < config->etcd.endpoints.size(); ++i) {
            if (i > 0) {
                endpoints += ",";
            }
            endpoints += config->etcd.endpoints[i];
        }
        auto instance = make_etcd_discovery(endpoints);
        return std::shared_ptr<IServiceDiscovery>(std::move(instance));
    }

    if (config->type != "local") {
        SHIELD_LOG_WARN << "Unknown discovery type '" << config->type
                        << "', falling back to local discovery";
    }

    auto instance = make_local_discovery(
        std::chrono::seconds(config->local.cleanup_interval_seconds),
        config->local.persistence_file_path);
    return std::shared_ptr<IServiceDiscovery>(std::move(instance));
}

}  // namespace shield::discovery
