#define BOOST_TEST_MODULE GatewayServiceIntegrationTest
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <memory>
#include <thread>

#include "shield/actor/distributed_actor_system.hpp"
#include "shield/config/config.hpp"
#include "shield/core/application_context.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/gateway/gateway_service.hpp"
#include "shield/log/logger.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/service/service_context.hpp"

using namespace shield;

struct GatewayIntegrationFixture {
    GatewayIntegrationFixture() {
        shield::log::LogConfig log_config;
        log_config.global_level =
            shield::log::Logger::level_from_string("info");
        shield::log::Logger::init(log_config);

        caf_cfg = std::make_unique<caf::actor_system_config>();
        system = std::make_unique<caf::actor_system>(*caf_cfg);

        auto discovery_unique = discovery::make_local_discovery();
        discovery_service = std::move(discovery_unique);

        actor::DistributedActorConfig actor_config;
        actor_config.node_id = "test_node";
        distributed_system = std::make_unique<actor::DistributedActorSystem>(
            *system, discovery_service, actor_config);
        distributed_system->initialize();

        script::LuaVMPoolConfig lua_config;
        lua_config.initial_size = 1;
        lua_config.min_size = 1;
        lua_config.max_size = 2;
        lua_vm_pool =
            std::make_unique<script::LuaVMPool>("test_lua_pool", lua_config);
        lua_vm_pool->on_init(core::ApplicationContext::instance());
        lua_vm_pool->on_start();

        svc_ctx = std::make_unique<service::ServiceContext>(
            *distributed_system, *system);
    }

    ~GatewayIntegrationFixture() {
        if (lua_vm_pool) {
            lua_vm_pool->on_stop();
        }
    }

    std::unique_ptr<caf::actor_system_config> caf_cfg;
    std::unique_ptr<caf::actor_system> system;
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service;
    std::unique_ptr<actor::DistributedActorSystem> distributed_system;
    std::unique_ptr<script::LuaVMPool> lua_vm_pool;
    std::unique_ptr<service::ServiceContext> svc_ctx;
};

BOOST_FIXTURE_TEST_SUITE(GatewayServiceIntegrationTests,
                         GatewayIntegrationFixture)

BOOST_AUTO_TEST_CASE(TestGatewayServiceLifecycle) {
    auto gateway_config =
        config::ConfigManager::instance()
            .get_configuration_properties<gateway::GatewayConfig>();
    if (!gateway_config) {
        gateway_config = std::make_shared<gateway::GatewayConfig>();
    }

    auto gateway = std::make_unique<gateway::GatewayService>(
        "test_gateway", *distributed_system, *lua_vm_pool, *svc_ctx,
        gateway_config);

    gateway->on_init(core::ApplicationContext::instance());
    BOOST_CHECK_EQUAL(gateway->name(), "test_gateway");

    gateway->on_start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    gateway->on_stop();
}

BOOST_AUTO_TEST_CASE(TestGatewayServiceConfiguration) {
    auto gateway_config =
        config::ConfigManager::instance()
            .get_configuration_properties<gateway::GatewayConfig>();
    if (!gateway_config) {
        gateway_config = std::make_shared<gateway::GatewayConfig>();
    }

    auto gateway = std::make_unique<gateway::GatewayService>(
        "config_test_gateway", *distributed_system, *lua_vm_pool, *svc_ctx,
        gateway_config);

    gateway->on_init(core::ApplicationContext::instance());
    BOOST_CHECK_EQUAL(gateway->name(), "config_test_gateway");

    gateway->on_stop();
}

BOOST_AUTO_TEST_SUITE_END()
