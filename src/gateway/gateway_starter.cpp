#include "shield/gateway/gateway_starter.hpp"

#include "shield/actor/distributed_actor_system.hpp"
#include "shield/config/config.hpp"
#include "shield/core/application_context.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/gateway/gateway_service.hpp"
#include "shield/log/logger.hpp"
#include "shield/script/lua_vm_pool.hpp"

namespace shield::gateway {

void GatewayStarter::initialize(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "Initializing Gateway Starter";

    // Get configuration properties
    auto& config_manager = config::ConfigManager::instance();
    auto gateway_config =
        config_manager.get_configuration_properties<GatewayConfig>();

    if (!gateway_config) {
        throw std::runtime_error(
            "GatewayConfig not found. Make sure it's registered.");
    }

    // Get required dependencies
    auto lua_vm_pool = context.get_service<script::LuaVMPool>();
    if (!lua_vm_pool) {
        throw std::runtime_error(
            "LuaVMPool not found. ScriptStarter must run first.");
    }

    auto actor_system = context.get_service<actor::DistributedActorSystem>();
    if (!actor_system) {
        throw std::runtime_error(
            "DistributedActorSystem not found. ActorStarter must run first.");
    }

    // Create GatewayService
    auto gateway_service = std::make_shared<GatewayService>(
        "gateway", *actor_system, *lua_vm_pool, gateway_config);

    // Register the service
    context.register_service("gateway", gateway_service);
    context.bind_config_reload<GatewayConfig>(gateway_service);

    SHIELD_LOG_INFO << "Gateway Starter initialized successfully";
}

}  // namespace shield::gateway
