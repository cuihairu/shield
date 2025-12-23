#include "shield/config/config_registry.hpp"

#include <mutex>

#include "shield/actor/actor_system_config.hpp"
#include "shield/config/config.hpp"
#include "shield/discovery/discovery_config.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/log_config.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/net/network_config.hpp"
#include "shield/script/lua_vm_pool_config.hpp"

namespace shield::config {

void register_all_configuration_properties() {
    static std::once_flag once;
    std::call_once(once, []() {
        ConfigurationPropertiesFactory<gateway::GatewayConfig>::
            create_and_register();
        ConfigurationPropertiesFactory<metrics::PrometheusConfig>::
            create_and_register();

        ConfigurationPropertiesFactory<net::TcpConfig>::create_and_register();
        ConfigurationPropertiesFactory<net::UdpConfig>::create_and_register();

        ConfigurationPropertiesFactory<actor::ActorSystemConfig>::
            create_and_register();
        ConfigurationPropertiesFactory<log::LogConfig>::create_and_register();

        ConfigurationPropertiesFactory<discovery::DiscoveryConfig>::
            create_and_register();
        ConfigurationPropertiesFactory<script::LuaVMPoolConfigProperties>::
            create_and_register();
    });
}

}  // namespace shield::config

