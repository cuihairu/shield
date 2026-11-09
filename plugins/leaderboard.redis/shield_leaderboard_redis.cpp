// [SHIELD_PLUGIN] leaderboard.redis — plugin entry: instance
// create, the shell wiring (get_interface/start/shutdown/
// register_lua), the process-wide instance registry, config
// parsing and the pool-stats surface (fabricated pool shapes would
// mislead dashboards, so every gauge reports unknown).
// Split out of shield_leaderboard_redis.cpp.
#include <sw/redis++/redis++.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/leaderboard.h"
#include "shield/plugin/pool_stats.h"
#include "shield/plugin/redis.h"
#include "shield_leaderboard_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json) and registers itself in a process-wide map so the Lua
// callable namespace `shield.leaderboard.redis(binding)` can resolve it.
// The C++ vtable is still served through get_interface() for host code that
// goes through LeaderboardPool / get_by_binding<shield_leaderboard_v1>.
//
// The Lua proxy goes straight to Redis via open_redis() (one fresh connection
// per call — redis++ pools internally) and uses the same encode_composite() /
// decode_composite() helpers the C vtable uses, so the composite-score
// encoding stays byte-identical across the C and Lua paths.
// ---------------------------------------------------------------------------

namespace {
// Process-wide registry: instance_id -> leaderboard_instance*. The callable
// Lua table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Map is read on every proxy creation, so it must be
// thread-safe.
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, leaderboard_instance*>& instances_map() {
    static std::map<std::string, leaderboard_instance*> m;
    return m;
}
}  // namespace

void register_instance(leaderboard_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
leaderboard_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract the known keys and fall
// back to defaults for anything missing.
void parse_instance_config(leaderboard_instance* inst,
                           const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string()) {
            inst->host = j["host"].get<std::string>();
        }
        if (j.contains("port") && j["port"].is_number_integer()) {
            inst->port = j["port"].get<int>();
        }
        if (j.contains("db") && j["db"].is_number_integer()) {
            inst->db = j["db"].get<int>();
        }
        if (j.contains("password") && j["password"].is_string()) {
            inst->password = j["password"].get<std::string>();
        }
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer()) {
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        }
        if (j.contains("command_timeout_ms") &&
            j["command_timeout_ms"].is_number_integer()) {
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
        }
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

namespace {
// shield.pool.stats.v1 — the Lua proxy opens one fresh Redis connection per
// call (open_redis()), so there is no persistent pool to observe: every
// gauge is -1 (unknown). Reporting a fabricated pool shape would mislead
// dashboards.
int lb_pool_get_stats(struct shield_plugin_instance_v1* self,
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
}  // namespace

const shield_pool_stats_v1& lb_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &lb_pool_get_stats,
    };
    return v;
}

namespace {
int lb_create(const shield_plugin_create_args_v1* args,
              shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new leaderboard_instance;
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
        if (iface && std::strcmp(iface, SHIELD_LEADERBOARD_INTERFACE) == 0)
            return &lb_vtable();
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0)
            return &lb_pool_stats_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1* self,
                           shield_error_v1*) -> int {
        auto* li = reinterpret_cast<leaderboard_instance*>(self);
        // Try to get redis.driver dependency (optional).
        if (li->host_api && li->host_api->dependency) {
            auto* drv = static_cast<const shield_redis_v2*>(
                li->host_api->dependency(li->ctx, "redis", SHIELD_REDIS_V2));
            if (drv && drv->connect) {
                char err_buf[256] = {};
                void* handle = drv->connect(drv, nullptr, err_buf,
                                            static_cast<int>(sizeof(err_buf)));
                if (handle) {
                    li->redis_driver = drv;
                    li->redis_handle = handle;
                }
            }
        }
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* li = reinterpret_cast<leaderboard_instance*>(self);
        if (li->redis_driver && li->redis_handle) {
            li->redis_driver->disconnect(li->redis_handle);
            li->redis_handle = nullptr;
            li->redis_driver = nullptr;
        }
        unregister_instance(li->instance_id);
        {
            std::lock_guard<std::mutex> lock(li->boards_mu);
            for (auto& [name, bc] : li->boards) {
                free_board_config_names(bc);
            }
            li->boards.clear();
        }
        delete li;
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
        "leaderboard.redis",
        "1.0.0",
        lb_create,
    };
    return &abi;
}
