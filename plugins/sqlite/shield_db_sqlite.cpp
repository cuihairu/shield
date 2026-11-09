// [SHIELD_PLUGIN] database.sqlite — plugin entry: instance create,
// the shell wiring (get_interface/start/shutdown/register_lua) and
// the extern "C" ABI export. The SQL executor, the async engine and
// the Lua facade live in the sibling translation units.
// Split out of shield_db_sqlite.cpp.
#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"
#include "shield_db_async_shim.hpp"
#include "shield_db_mapper.hpp"
#include "shield_db_sqlite_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json) and registers itself in a process-wide map so the Lua callable
// namespace can resolve plugins.bindings logical names to started instances.
// The C++ vtable is still served through get_interface() for any C-ABI
// caller (none in tree today, but kept for forward compatibility).
// ---------------------------------------------------------------------------

namespace {
int sqlite_create(const shield_plugin_create_args_v1* args,
                  shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new sqlite_instance;
    inst->host_api = args ? args->host_api : nullptr;
    inst->ctx = args ? args->ctx : nullptr;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";
    parse_instance_config(inst, args ? args->config_json : nullptr);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_DATABASE_INTERFACE) == 0)
            return &db_vtable();
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0)
            return &sqlite_pool_stats_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        // shell is the first member of sqlite_instance (offset 0), so self
        // points at the enclosing sqlite_instance. Standard C-ABI pattern.
        auto* inst = reinterpret_cast<sqlite_instance*>(self);
        // Drain the async worker BEFORE the instance dies: stop the queue,
        // wake, join. In-flight SQL still finishes; its completion goes to
        // the host, which drops it (caller gone) — never a use-after-free.
        {
            std::lock_guard<std::mutex> lk(inst->worker_mu);
            inst->worker_stop = true;
        }
        inst->worker_cv.notify_all();
        if (inst->worker_thread.joinable()) {
            inst->worker_thread.join();
        }
        // Roll back transactions still holding a handle: the worker is
        // joined, so nothing else can end them on their own.
        drain_held_tx(inst);
        unregister_instance(inst->instance_id);
        delete inst;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}
}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.sqlite",
        "1.0.0",
        sqlite_create,
    };
    return &abi;
}
