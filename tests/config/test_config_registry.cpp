#define BOOST_TEST_MODULE ConfigRegistryTest
#include <boost/test/unit_test.hpp>

#include "shield/actor/actor_system_config.hpp"
#include "shield/config/config.hpp"
#include "shield/config/config_registry.hpp"
#include "shield/discovery/discovery_config.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/log/log_config.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/net/network_config.hpp"
#include "shield/script/lua_vm_pool_config.hpp"

using namespace shield::config;

BOOST_AUTO_TEST_SUITE(ConfigRegistryTests)

BOOST_AUTO_TEST_CASE(TestRegisterAllConfigurationProperties) {
    // Should not throw, and should be idempotent
    BOOST_CHECK_NO_THROW(register_all_configuration_properties());
    BOOST_CHECK_NO_THROW(register_all_configuration_properties());
}

BOOST_AUTO_TEST_CASE(TestGatewayConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::gateway::GatewayConfig>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_CASE(TestPrometheusConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::metrics::PrometheusConfig>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_CASE(TestTcpConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::net::TcpConfig>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_CASE(TestUdpConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::net::UdpConfig>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_CASE(TestActorSystemConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::actor::ActorSystemConfig>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_CASE(TestLogConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::log::LogConfig>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_CASE(TestDiscoveryConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::discovery::DiscoveryConfig>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_CASE(TestLuaVMPoolConfigRegistered) {
    register_all_configuration_properties();
    auto& cm = ConfigManager::instance();
    auto cfg = cm.get_configuration_properties<shield::script::LuaVMPoolConfigProperties>();
    BOOST_CHECK(cfg != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
