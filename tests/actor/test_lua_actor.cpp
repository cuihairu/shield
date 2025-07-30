// Test file for LuaActor
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/init_global_meta_objects.hpp"  // CAF initialization
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/actor/lua_actor.hpp"
#include "shield/caf_type_ids.hpp"  // Include CAF type IDs
#include "shield/core/application_context.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/log/logger.hpp"
#include "shield/script/lua_vm_pool.hpp"

class TestLuaActor : public shield::actor::LuaActor {
public:
    TestLuaActor(caf::actor_config &cfg, shield::script::LuaVMPool &lua_vm_pool,
                 shield::actor::DistributedActorSystem &actor_system,
                 const std::string &script_path)
        : LuaActor(cfg, lua_vm_pool, actor_system, script_path, "test_actor") {}

    // Expose protected methods for testing
    using LuaActor::load_script;
    using LuaActor::process_message;
};

void test_lua_actor() {
    // Initialize logging
    shield::log::LogConfig log_config;
    shield::log::Logger::init(log_config);

    // Initialize CAF global meta objects - REQUIRED before actor_system
    // creation
    caf::core::init_global_meta_objects();

    std::cout << "=== Testing LuaActor with Player Script ===" << std::endl;

    // Ensure script directory exists
    std::filesystem::create_directories("scripts");

    std::string script_path = "scripts/player_actor.lua";
    if (!std::filesystem::exists(script_path)) {
        std::cerr << "Script file not found: " << script_path << std::endl;
        std::cerr
            << "Please make sure player_actor.lua exists in scripts/ directory"
            << std::endl;
        std::cerr
            << "Skipping actual script test, but testing actor creation..."
            << std::endl;
        // Create a dummy script file for testing
        std::ofstream dummy_script(script_path);
        dummy_script << "-- Dummy test script\n";
        dummy_script << "function handle_message(message_type, data)\n";
        dummy_script << "    return true, {result = 'test'}\n";
        dummy_script << "end\n";
        dummy_script.close();
    }

    try {
        // Setup CAF actor system
        caf::actor_system_config caf_cfg;
        caf::actor_system system{caf_cfg};

        // Setup service discovery
        auto discovery_service = shield::discovery::make_local_discovery();
        std::shared_ptr<shield::discovery::IServiceDiscovery> discovery_ptr =
            std::move(discovery_service);

        // Setup distributed actor system
        shield::actor::DistributedActorConfig actor_config;
        actor_config.node_id = "test_node_123";
        shield::actor::DistributedActorSystem distributed_actor_system(
            system, discovery_ptr, actor_config);
        distributed_actor_system.initialize();

        // Setup Lua VM Pool
        shield::script::LuaVMPoolConfig lua_config;
        lua_config.initial_size = 1;
        lua_config.min_size = 1;
        lua_config.max_size = 2;
        shield::script::LuaVMPool lua_vm_pool("test_pool", lua_config);
        auto &ctx = shield::core::ApplicationContext::instance();
        lua_vm_pool.on_init(ctx);
        lua_vm_pool.on_start();

        // Create test actor using CAF spawn
        auto actor_handle = system.spawn<TestLuaActor>(
            std::ref(lua_vm_pool), std::ref(distributed_actor_system),
            script_path);

        std::cout << "✅ LuaActor created successfully" << std::endl;

        // Note: Since this is now a proper CAF actor, we would need to use
        // CAF's message passing mechanism to test it properly.
        // For now, we'll just verify that the actor can be created.

        std::cout << "\n🎉 LuaActor creation test passed!" << std::endl;
        std::cout << "Note: Full message testing requires CAF message passing"
                  << std::endl;

        // Cleanup
        lua_vm_pool.on_stop();

    } catch (const std::exception &e) {
        std::cerr << "Test setup failed: " << e.what() << std::endl;
        throw;
    }
}

int main() {
    try {
        test_lua_actor();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}