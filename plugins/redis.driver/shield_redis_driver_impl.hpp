// [SHIELD_PLUGIN] redis.driver — internal cross-TU declarations
// for the plugin split.
//
// Not a public contract: the exported ABI stays in
// shield/plugin/redis.h. The instance struct moves here verbatim so
// the shield_redis_driver*.cpp translation units share one layout;
// the file-local helpers shared across the split are defined flat
// (external linkage) in their own TU, one declaration each.
#pragma once

#include <memory>
#include <string>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"

struct redis_instance {
    shield_plugin_instance_v1 shell;
    // Per-instance shield.redis.v2 vtable. Each instance owns its own copy so
    // that get_interface() returns an instance-bound vtable and connect()
    // can recover THIS instance from the self pointer (multi-instance safe).
    shield_redis_v2 vtable_{};
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;

    // Parsed config
    std::string mode = "single";
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int pool_size = 8;
    int connect_timeout_ms = 5000;
    int command_timeout_ms = 3000;

    // The redis++ client, created in start().
    std::shared_ptr<sw::redis::Redis> redis;
};

// Defined in shield_redis_driver_exec.cpp: fills the typed vtable
// (called once per instance by redis_driver_create).
void init_vtable(shield_redis_v2* out);

// Defined in shield_redis_driver_exec.cpp: the process-wide registry
// (create registers, shutdown unregisters, the Lua facade resolves).
void register_instance(redis_instance* inst);
void unregister_instance(const std::string& id);
redis_instance* find_instance(const std::string& id);

// Defined in shield_redis_driver_lua.cpp: register_lua implementation,
// installed by redis_driver_create on the shell.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);
