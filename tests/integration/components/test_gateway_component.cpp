#define BOOST_TEST_MODULE GatewayComponentIntegrationTest
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <thread>
#include <memory>

#include "shield/gateway/gateway_component.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/core/logger.hpp"
#include "shield/discovery/local_discovery.hpp"

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
    void init() {}
    void start() {}
    void stop() {}
    std::string name() const { return "mock_lua_vm_pool"; }
};

struct GatewayIntegrationFixture {
    GatewayIntegrationFixture() {
        // Initialize logger
        core::LogConfig log_config;
        log_config.level = core::Logger::level_from_string("info");
        core::Logger::init(log_config);
        
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
        lua_vm_pool = std::make_unique<script::LuaVMPool>("test_lua_pool", lua_config);
        lua_vm_pool->init();
        lua_vm_pool->start();
    }
    
    ~GatewayIntegrationFixture() {
        if (lua_vm_pool) {
            lua_vm_pool->stop();
        }
    }
    
    std::unique_ptr<caf::actor_system_config> caf_cfg;
    std::unique_ptr<caf::actor_system> system;
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service;
    std::unique_ptr<actor::DistributedActorSystem> distributed_system;
    std::unique_ptr<script::LuaVMPool> lua_vm_pool;
};

BOOST_FIXTURE_TEST_SUITE(GatewayComponentIntegrationTests, GatewayIntegrationFixture)

BOOST_AUTO_TEST_CASE(TestGatewayComponentLifecycle) {
    // Create gateway component
    auto gateway = std::make_unique<gateway::GatewayComponent>(
        "test_gateway", *distributed_system, *lua_vm_pool);
    
    // Test initialization
    gateway->init();
    BOOST_CHECK_EQUAL(gateway->name(), "test_gateway");
    
    // Test start
    gateway->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Test stop
    gateway->stop();
}

BOOST_AUTO_TEST_CASE(TestGatewayComponentConfiguration) {
    auto gateway = std::make_unique<gateway::GatewayComponent>(
        "config_test_gateway", *distributed_system, *lua_vm_pool);
    
    gateway->init();
    
    // Verify component is properly configured
    BOOST_CHECK_EQUAL(gateway->name(), "config_test_gateway");
    
    gateway->stop();
}

BOOST_AUTO_TEST_SUITE_END()