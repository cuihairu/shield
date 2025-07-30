#include "shield/actor/actor_system_config.hpp"
#include "shield/config/module_config.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/log_config.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/net/network_config.hpp"

// 自动注册所有模块配置
namespace shield::config {

void register_all_module_configs() {
    // 注册Gateway配置
    ModuleConfigFactory<gateway::GatewayConfig>::create_and_register();

    // 注册Prometheus配置
    ModuleConfigFactory<metrics::PrometheusConfig>::create_and_register();

    // 注册网络模块配置
    ModuleConfigFactory<net::TcpConfig>::create_and_register();
    ModuleConfigFactory<net::UdpConfig>::create_and_register();

    // 注册Actor系统配置
    ModuleConfigFactory<actor::ActorSystemConfig>::create_and_register();

    // 注册日志模块配置
    ModuleConfigFactory<log::LogConfig>::create_and_register();
}

// 静态初始化，程序启动时自动调用
namespace {
[[maybe_unused]] static auto _ = []() {
    register_all_module_configs();
    return 0;
}();
}  // namespace

}  // namespace shield::config