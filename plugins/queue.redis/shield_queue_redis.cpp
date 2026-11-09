// [SHIELD_PLUGIN] queue.redis — plugin entry: instance create
// (config parsing, driver/own-connection wiring), the shell
// wiring (get_interface/start/shutdown/register_lua), the
// process-wide instance registry and the pool-stats surface
// (no persistent pool, so every gauge reports unknown).
// Split out of shield_queue_redis.cpp.
#include <sw/redis++/redis++.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"
#include "shield/plugin/queue.h"
#include "shield/plugin/redis.h"
#include "shield_lua_plugin_binding.hpp"
#include "shield_queue_redis_impl.hpp"

namespace {
// Process-wide registry
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, queue_instance*>& instances_map() {
    static std::map<std::string, queue_instance*> m;
    return m;
}
}  // namespace

void register_instance(queue_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
queue_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

namespace {
void parse_instance_config(queue_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string())
            inst->host = j["host"].get<std::string>();
        if (j.contains("port") && j["port"].is_number_integer())
            inst->port = j["port"].get<int>();
        if (j.contains("db") && j["db"].is_number_integer())
            inst->db = j["db"].get<int>();
        if (j.contains("password") && j["password"].is_string())
            inst->password = j["password"].get<std::string>();
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer())
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        if (j.contains("command_timeout_ms") &&
            j["command_timeout_ms"].is_number_integer())
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
    } catch (...) {
    }
}
}  // namespace

namespace {
// shield.pool.stats.v1 — queue.redis opens one fresh sw::redis::Redis
// connection per operation and keeps no persistent pool, so every gauge is
// -1 (unknown); reporting a fabricated pool shape would mislead dashboards.
int queue_pool_get_stats(struct shield_plugin_instance_v1* self,
                         struct shield_pool_stats* out) {
    (void)self;
    if (!out) return -1;
    out->max_size = -1;
    out->size = -1;
    out->idle = -1;
    out->in_use = -1;
    out->waiters = -1;
    out->acquire_timeout_total = -1;
    out->acquire_total = -1;
    out->create_total = -1;
    out->destroy_total = -1;
    out->eviction_total = -1;
    out->health_check_failures_total = -1;
    out->last_error_epoch_ms = -1;
    return 0;
}

const shield_pool_stats_v1& queue_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &queue_pool_get_stats,
    };
    return v;
}
}  // namespace

namespace {
int queue_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new queue_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_QUEUE_INTERFACE) == 0)
            return &queue_vtable();
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0)
            return &queue_pool_stats_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1* self,
                           shield_error_v1*) -> int {
        auto* qi = reinterpret_cast<queue_instance*>(self);
        if (qi->host_api && qi->host_api->dependency) {
            auto* drv = static_cast<const shield_redis_v2*>(
                qi->host_api->dependency(qi->ctx, "redis", SHIELD_REDIS_V2));
            if (drv && drv->connect) {
                char err_buf[256] = {};
                void* handle =
                    drv->connect(drv, nullptr, err_buf, sizeof(err_buf));
                if (handle) {
                    qi->redis_driver = drv;
                    qi->redis_handle = handle;
                }
            }
        }
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* qi = reinterpret_cast<queue_instance*>(self);
        qi->stop_all();
        if (qi->redis_driver && qi->redis_handle) {
            qi->redis_driver->disconnect(qi->redis_handle);
            qi->redis_handle = nullptr;
            qi->redis_driver = nullptr;
        }
        unregister_instance(qi->instance_id);
        delete qi;
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
        "queue.redis",
        "1.0.0",
        queue_create,
    };
    return &abi;
}
