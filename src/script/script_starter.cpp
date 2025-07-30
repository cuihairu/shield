#include "shield/script/script_starter.hpp"

#include "shield/core/application_context.hpp"
#include "shield/log/logger.hpp"
#include "shield/script/lua_vm_pool.hpp"

namespace shield::script {

void ScriptStarter::initialize(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "Initializing Script Starter";

    // Create LuaVMPool with default configuration
    LuaVMPoolConfig lua_config;
    lua_config.initial_size = 4;
    lua_config.max_size = 16;
    lua_config.min_size = 2;

    auto lua_vm_pool = std::make_shared<LuaVMPool>("main_pool", lua_config);

    // Register the LuaVMPool service
    context.register_service("lua_vm_pool", lua_vm_pool);

    SHIELD_LOG_INFO << "Script Starter initialized successfully";
}

}  // namespace shield::script