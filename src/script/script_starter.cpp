#include "shield/script/script_starter.hpp"

#include "shield/config/config.hpp"
#include "shield/core/application_context.hpp"
#include "shield/log/logger.hpp"
#include "shield/script/lua_vm_pool.hpp"
#include "shield/script/lua_vm_pool_config.hpp"

namespace shield::script {

void ScriptStarter::initialize(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "Initializing Script Starter";

    auto& config_manager = config::ConfigManager::instance();
    auto pool_properties =
        config_manager
            .get_configuration_properties<LuaVMPoolConfigProperties>();

    LuaVMPoolConfig lua_config =
        pool_properties ? pool_properties->to_pool_config() : LuaVMPoolConfig{};

    auto lua_vm_pool = std::make_shared<LuaVMPool>("lua_vm_pool", lua_config);

    if (pool_properties) {
        for (const auto& path : pool_properties->script_paths) {
            lua_vm_pool->preload_script(path);
        }
    }

    // Register the LuaVMPool service
    context.register_service("lua_vm_pool", lua_vm_pool);

    SHIELD_LOG_INFO << "Script Starter initialized successfully";
}

}  // namespace shield::script
