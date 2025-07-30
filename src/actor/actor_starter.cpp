#include "shield/actor/actor_starter.hpp"

#include <random>

#include "caf/actor_system.hpp"
#include "caf/io/all.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/core/application_context.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/log/logger.hpp"

namespace shield::actor {

void ActorStarter::initialize(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "Initializing Actor Starter";

    // Create default actor system configuration
    DistributedActorConfig actor_config;
    actor_config.node_id =
        "shield_node_" + std::to_string(std::random_device{}());
    actor_config.cluster_name = "shield_cluster";
    actor_config.actor_port = 0;  // Let system choose port

    // Create DistributedActorSystem using the constructor that takes name and
    // config
    auto actor_system =
        std::make_shared<DistributedActorSystem>("actor_system", actor_config);

    // Register the service
    context.register_service("actor_system", actor_system);

    SHIELD_LOG_INFO << "Actor Starter initialized successfully";
}

}  // namespace shield::actor