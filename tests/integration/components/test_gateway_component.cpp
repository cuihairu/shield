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

using namespace shield;

class MockDistributedActorSystem {
public:
    MockDistributedActorSystem() {
        // Mock implementation
    }

    void initialize() {
        // Mock initialization
    }

    // Add other required methods as mocks
};

class MockLuaVMPool {
public:
    MockLuaVMPool() = default;

    // Mock VM pool methods
    void on_init(core::ApplicationContext& ctx) {}
    void on_start() {}
    void on_stop() {}
    std::string name() const { return "mock_lua_vm_pool"; }
};

struct GatewayIntegrationFixture {
    GatewayIntegrationFixture() {
        // Initialize logger
        shield::log::LogConfig log_config;
        log_config.global_level =
            shield::log::Logger::level_from_string("info");
        shield::log::Logger::init(log_config);

        // Initialize CAF system
        caf_cfg = std::make_unique<caf::actor_system_config>();
        system = std::make_unique<caf::actor_system>(*caf_cfg);

        // Create discovery service
        auto discovery_unique = discovery::make_local_discovery();
        discovery_service = std::move(discovery_unique);

        // Create distributed actor system
        actor::DistributedActorConfig actor_config;
        actor_config.node_id = "test_node";
        distributed_system = std::make_unique<actor::DistributedActorSystem>(
            *system, discovery_service, actor_config);
        distributed_system->initialize();

        // Create Lua VM pool
        script::LuaVMPoolConfig lua_config;
        lua_config.initial_size = 1;
        lua_config.min_size = 1;
        lua_config.max_size = 2;
        lua_vm_pool =
            std::make_unique<script::LuaVMPool>("test_lua_pool", lua_config);
        lua_vm_pool->on_init(core::ApplicationContext::instance());
        lua_vm_pool->on_start();
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
};

BOOST_FIXTURE_TEST_SUITE(GatewayServiceIntegrationTests,
                         GatewayIntegrationFixture)

BOOST_AUTO_TEST_CASE(TestGatewayServiceLifecycle) {
    // Create gateway service
    auto gateway = std::make_unique<gateway::GatewayService>(
        "test_gateway", *distributed_system, *lua_vm_pool,
        config::ConfigManager::instance()
            .get_component_config<gateway::GatewayConfig>());

    // Test initialization
    gateway->on_init(core::ApplicationContext::instance());
    BOOST_CHECK_EQUAL(gateway->name(), "test_gateway");

    // Test start
    gateway->on_start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Test stop
    gateway->on_stop();
}

BOOST_AUTO_TEST_CASE(TestGatewayServiceConfiguration) {
    auto gateway = std::make_unique<gateway::GatewayService>(
        "config_test_gateway", *distributed_system, *lua_vm_pool,
        config::ConfigManager::instance()
            .get_component_config<gateway::GatewayConfig>());

    gateway->on_init(core::ApplicationContext::instance());

    // Verify service is properly configured
    BOOST_CHECK_EQUAL(gateway->name(), "config_test_gateway");

    gateway->on_stop();
}

BOOST_AUTO_TEST_SUITE_END()