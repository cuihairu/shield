#include "shield/script/lua_vm_pool_config.hpp"

#include <stdexcept>

namespace shield::script {

void LuaVMPoolConfigProperties::from_ptree(
    const boost::property_tree::ptree& pt) {
    initial_size = get_value(pt, "initial_size", initial_size);
    min_size = get_value(pt, "min_size", min_size);
    max_size = get_value(pt, "max_size", max_size);
    idle_timeout_ms = get_value(pt, "idle_timeout_ms", idle_timeout_ms);
    acquire_timeout_ms =
        get_value(pt, "acquire_timeout_ms", acquire_timeout_ms);
    preload_scripts = get_value(pt, "preload_scripts", preload_scripts);

    load_vector(pt, "script_paths", script_paths);
}

void LuaVMPoolConfigProperties::validate() const {
    if (min_size > max_size) {
        throw std::invalid_argument("lua_vm_pool.min_size must be <= max_size");
    }
    if (initial_size < min_size || initial_size > max_size) {
        throw std::invalid_argument(
            "lua_vm_pool.initial_size must be within [min_size, max_size]");
    }
    if (idle_timeout_ms <= 0) {
        throw std::invalid_argument("lua_vm_pool.idle_timeout_ms must be > 0");
    }
    if (acquire_timeout_ms <= 0) {
        throw std::invalid_argument(
            "lua_vm_pool.acquire_timeout_ms must be > 0");
    }
}

LuaVMPoolConfig LuaVMPoolConfigProperties::to_pool_config() const {
    LuaVMPoolConfig cfg;
    cfg.initial_size = initial_size;
    cfg.max_size = max_size;
    cfg.min_size = min_size;
    cfg.idle_timeout = std::chrono::milliseconds{idle_timeout_ms};
    cfg.acquire_timeout = std::chrono::milliseconds{acquire_timeout_ms};
    cfg.preload_scripts = preload_scripts;
    return cfg;
}

}  // namespace shield::script
