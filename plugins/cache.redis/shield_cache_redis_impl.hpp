// [SHIELD_PLUGIN] cache.redis — internal cross-TU declarations
// for the plugin split.
//
// Not a public contract: the exported ABI stays in
// shield/plugin/cache.h. The conn struct (global scope in the
// original for lambda-to-fnptr conversion) and the instance struct
// move here verbatim so the shield_cache_redis*.cpp translation
// units share one layout; the file-local helpers shared across the
// split are defined flat (external linkage) in their own TU, one
// declaration each.
#pragma once

#include <sw/redis++/redis++.h>

#include <memory>
#include <string>

#include "shield/plugin/abi.h"
#include "shield/plugin/cache.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"

struct cache_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    // Parsed config (mirrors shield_cache_config keys).
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int pool_size = 8;
    int connect_timeout_ms = 5000;
    int command_timeout_ms = 5000;
    // Optional redis.driver dependency (Phase 2 migration).
    const shield_redis_v2* redis_driver = nullptr;
    void* redis_handle = nullptr;  // opaque handle from redis_driver->connect()
};

// Defined in shield_cache_redis_exec.cpp: the shared connection
// builder used by both the C vtable (connect) and the Lua instance
// overload below.
std::shared_ptr<sw::redis::Redis> open_redis(const std::string& host, int port,
                                             const std::string& password,
                                             int db, int pool_size,
                                             int connect_timeout_ms,
                                             int command_timeout_ms,
                                             std::string* err);

// Defined in shield_cache_redis_exec.cpp: the legacy C-ABI surface,
// served through get_interface() in cache_create.
const shield_cache_v1& cache_vtable();

// Defined in shield_cache_redis.cpp: the process-wide registry.
void register_instance(cache_instance* inst);
void unregister_instance(const std::string& id);
cache_instance* find_instance(const std::string& id);

// Defined in shield_cache_redis_lua.cpp: register_lua implementation,
// installed by cache_create on the shell.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);
