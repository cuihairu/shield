#include "shield/actor/actor_system_config.hpp"
#include "shield/config/config.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/log_config.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/net/network_config.hpp"

// 自动注册所有组件配置
namespace shield::config {

void register_all_component_configs() {
    // 注册Gateway配置
    ComponentConfigFactory<gateway::GatewayConfig>::create_and_register();

    // 注册Prometheus配置
    ComponentConfigFactory<metrics::PrometheusConfig>::create_and_register();

    // 注册网络组件配置
    ComponentConfigFactory<net::TcpConfig>::create_and_register();
    ComponentConfigFactory<net::UdpConfig>::create_and_register();

    // 注册Actor系统配置
    ComponentConfigFactory<actor::ActorSystemConfig>::create_and_register();

    // 注册日志组件配置
    ComponentConfigFactory<log::LogConfig>::create_and_register();
}

// 静态初始化，程序启动时自动调用
namespace {
[[maybe_unused]] static auto _ = []() {
    register_all_component_configs();
    return 0;
}();
}  // namespace

}  // namespace shield::config