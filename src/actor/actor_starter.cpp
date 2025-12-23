#include "shield/actor/actor_starter.hpp"

#include <chrono>
#include <random>

#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/io/middleman.hpp"
#include "shield/actor/actor_system_config.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/core/application_context.hpp"
#include "shield/discovery/consul_discovery.hpp"
#include "shield/discovery/discovery_config.hpp"
#include "shield/discovery/etcd_discovery.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/discovery/nacos_discovery.hpp"
#include "shield/discovery/redis_discovery.hpp"
#include "shield/log/logger.hpp"

namespace shield::actor {

void ActorStarter::initialize(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "Initializing Actor Starter";

    initialize_caf_types();

    auto& config_manager = config::ConfigManager::instance();
    auto actor_system_config =
        config_manager.get_configuration_properties<ActorSystemConfig>();
    auto discovery_config =
        config_manager
            .get_configuration_properties<discovery::DiscoveryConfig>();

    size_t worker_threads = 0;
    std::string node_id =
        "shield_node_" + std::to_string(std::random_device{}());
    std::string cluster_name = "shield_cluster";
    uint16_t actor_port = 0;

    if (actor_system_config) {
        worker_threads = static_cast<size_t>(
            actor_system_config->get_effective_worker_threads());
        node_id = actor_system_config->get_effective_node_id();
        cluster_name = actor_system_config->node.cluster_name;
        actor_port = actor_system_config->network.port;
    }

    caf::actor_system_config caf_config;
    if (worker_threads > 0) {
        caf_config.set("scheduler.max-threads", worker_threads);
    }
    caf_config.set("logger.verbosity", caf::log::level::info);
    caf_config.load<caf::io::middleman>();

    auto caf_system = context.register_bean<caf::actor_system>(
        "caf_actor_system", caf_config);

    std::shared_ptr<discovery::IServiceDiscovery> discovery_service;
    if (discovery_config) {
        if (discovery_config->type == "redis") {
            auto instance = discovery::make_redis_discovery(
                discovery_config->build_redis_uri(),
                discovery_config->redis_heartbeat_interval());
            discovery_service = std::shared_ptr<discovery::IServiceDiscovery>(
                std::move(instance));
        } else if (discovery_config->type == "nacos") {
            auto instance = discovery::make_nacos_discovery(
                discovery_config->nacos.server_addr,
                std::chrono::seconds(
                    discovery_config->nacos.heartbeat_interval_seconds));
            discovery_service = std::shared_ptr<discovery::IServiceDiscovery>(
                std::move(instance));
        } else if (discovery_config->type == "consul") {
            auto instance = discovery::make_consul_discovery(
                discovery_config->consul.host + ":" +
                    std::to_string(discovery_config->consul.port),
                std::chrono::seconds(
                    discovery_config->consul.check_interval_seconds));
            discovery_service = std::shared_ptr<discovery::IServiceDiscovery>(
                std::move(instance));
        } else if (discovery_config->type == "etcd") {
            std::string endpoints;
            for (size_t i = 0; i < discovery_config->etcd.endpoints.size();
                 ++i) {
                if (i > 0) endpoints += ",";
                endpoints += discovery_config->etcd.endpoints[i];
            }
            auto instance = discovery::make_etcd_discovery(endpoints);
            discovery_service = std::shared_ptr<discovery::IServiceDiscovery>(
                std::move(instance));
        } else {
            auto instance = discovery::make_local_discovery(
                std::chrono::seconds(
                    discovery_config->local.cleanup_interval_seconds),
                discovery_config->local.persistence_file_path);
            discovery_service = std::shared_ptr<discovery::IServiceDiscovery>(
                std::move(instance));
        }
    } else {
        auto instance = discovery::make_local_discovery();
        discovery_service =
            std::shared_ptr<discovery::IServiceDiscovery>(std::move(instance));
    }

    DistributedActorConfig dist_config;
    dist_config.node_id = node_id;
    dist_config.cluster_name = cluster_name;
    dist_config.actor_port = actor_port;

    auto actor_system = std::make_shared<DistributedActorSystem>(
        *caf_system, std::move(discovery_service), dist_config);

    // Register the service
    context.register_service("actor_system", actor_system);

    SHIELD_LOG_INFO << "Actor Starter initialized successfully";
}

}  // namespace shield::actor
