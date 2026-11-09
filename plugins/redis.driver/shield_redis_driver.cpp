// [SHIELD_PLUGIN] redis.driver — plugin entry: instance create
// (config parsing, redis++ pool construction), the shell wiring
// (get_interface/start/shutdown/register_lua) and extern "C"
// get_v1.
// Split out of shield_redis_driver.cpp.
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"
#include "shield_lua_plugin_binding.hpp"
#include "shield_redis_driver_impl.hpp"

// ---------------------------------------------------------------------------
// Instance config
// ---------------------------------------------------------------------------

namespace {
void parse_instance_config(redis_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("mode") && j["mode"].is_string())
            inst->mode = j["mode"].get<std::string>();
        if (j.contains("host") && j["host"].is_string())
            inst->host = j["host"].get<std::string>();
        if (j.contains("port") && j["port"].is_number_integer())
            inst->port = j["port"].get<int>();
        if (j.contains("password") && j["password"].is_string())
            inst->password = j["password"].get<std::string>();
        if (j.contains("db") && j["db"].is_number_integer())
            inst->db = j["db"].get<int>();
        if (j.contains("pool_size") && j["pool_size"].is_number_integer())
            inst->pool_size = j["pool_size"].get<int>();
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer())
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        if (j.contains("command_timeout_ms") &&
            j["command_timeout_ms"].is_number_integer())
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
    } catch (...) {
        // Host already validated config; ignore parse errors.
    }
}
}  // namespace

namespace {
// Build a sw::redis::Redis from the instance config.
bool open_redis(redis_instance* inst, std::string* err) {
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = inst->host.empty() ? "localhost" : inst->host;
        opts.port = inst->port > 0 ? inst->port : 6379;
        if (!inst->password.empty()) opts.password = inst->password;
        opts.db = inst->db > 0 ? inst->db : 0;
        opts.connect_timeout = std::chrono::milliseconds(
            inst->connect_timeout_ms > 0 ? inst->connect_timeout_ms : 5000);
        opts.socket_timeout = std::chrono::milliseconds(
            inst->command_timeout_ms > 0 ? inst->command_timeout_ms : 3000);

        sw::redis::ConnectionPoolOptions pool_opts;
        pool_opts.size =
            inst->pool_size > 0 ? static_cast<std::size_t>(inst->pool_size) : 8;

        inst->redis = std::make_shared<sw::redis::Redis>(opts, pool_opts);
        inst->redis->ping();
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// v1 ABI entry
// ---------------------------------------------------------------------------

namespace {
int redis_driver_create(const shield_plugin_create_args_v1* args,
                        shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new redis_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    init_vtable(&inst->vtable_);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_REDIS_V2) == 0)
            return &reinterpret_cast<redis_instance*>(self)->vtable_;
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1* self,
                           shield_error_v1* err) -> int {
        auto* ri = reinterpret_cast<redis_instance*>(self);
        std::string open_err;
        if (!open_redis(ri, &open_err)) {
            if (err) {
                err->code = "redis.connect.failed";
                err->message = open_err.c_str();
            }
            return -1;
        }
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* ri = reinterpret_cast<redis_instance*>(self);
        unregister_instance(ri->instance_id);
        ri->redis.reset();
        delete ri;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}
}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "redis.driver",
        "1.0.0",
        redis_driver_create,
    };
    return &abi;
}
