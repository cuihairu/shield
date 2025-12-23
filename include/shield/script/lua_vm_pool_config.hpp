#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/script/lua_vm_pool.hpp"

namespace shield::script {

class LuaVMPoolConfigProperties
    : public config::ReloadableConfigurationProperties<LuaVMPoolConfigProperties> {
public:
    size_t initial_size = 4;
    size_t max_size = 16;
    size_t min_size = 2;
    int idle_timeout_ms = 30000;
    int acquire_timeout_ms = 5000;
    bool preload_scripts = true;
    std::vector<std::string> script_paths;

    void from_ptree(const boost::property_tree::ptree& pt) override;
    void validate() const override;
    std::string properties_name() const override { return "lua_vm_pool"; }

    LuaVMPoolConfig to_pool_config() const;
};

}  // namespace shield::script

