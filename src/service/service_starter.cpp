#include "shield/service/service_starter.hpp"

#include "shield/actor/distributed_actor_system.hpp"
#include "shield/core/application_context.hpp"
#include "shield/log/logger.hpp"
#include "shield/service/console.hpp"
#include "shield/service/service_context.hpp"

namespace shield::service {

void ServiceStarter::initialize(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "Initializing Service Starter";

    auto dist_system = context.get_service<actor::DistributedActorSystem>();
    if (!dist_system) {
        throw std::runtime_error(
            "DistributedActorSystem not found. ActorStarter must run first.");
    }

    auto caf_system = context.get_bean<caf::actor_system>("caf_actor_system");
    if (!caf_system) {
        throw std::runtime_error(
            "caf::actor_system bean not found. ActorStarter must run first.");
    }

    auto svc_ctx = std::make_shared<ServiceContext>(*dist_system, *caf_system);
    context.register_bean<ServiceContext>("service_context", svc_ctx);

    auto console = std::make_shared<DebugConsole>("debug_console", 13000);
    context.register_service("debug_console", console);

    SHIELD_LOG_INFO << "Service Starter initialized successfully";
}

}  // namespace shield::service
