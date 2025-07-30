#include "shield/actor/actor_system_config.hpp"
#include "shield/config/config.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/log_config.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/net/network_config.hpp"

// Auto-register all component configurations
namespace shield::config {

void register_all_component_configs() {
    // Register Gateway configuration
    ComponentConfigFactory<gateway::GatewayConfig>::create_and_register();

    // Register Prometheus configuration
    ComponentConfigFactory<metrics::PrometheusConfig>::create_and_register();

    // Register network component configurations
    ComponentConfigFactory<net::TcpConfig>::create_and_register();
    ComponentConfigFactory<net::UdpConfig>::create_and_register();

    // Register Actor system configuration
    ComponentConfigFactory<actor::ActorSystemConfig>::create_and_register();

    // Register log component configuration
    ComponentConfigFactory<log::LogConfig>::create_and_register();
}

// Static initialization, automatically called at program startup
namespace {
[[maybe_unused]] static auto _ = []() {
    register_all_component_configs();
    return 0;
}();
}  // namespace

}  // namespace shield::config