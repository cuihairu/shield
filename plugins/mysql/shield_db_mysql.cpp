// [SHIELD_PLUGIN] database.mysql — plugin entry: instance lifecycle
// (mysql_create) and the get_v1 export. Executor, async engine and
// Lua facade live in the sibling translation units.
//
// v1 ABI (shield_plugin_get_v1). Built on MariaDB Connector/C (libmariadb),
// which speaks the classic MySQL client/server protocol and therefore works
// against both MySQL and MariaDB servers. Statements run through the prepared
// statement API (mysql_stmt_*) so `?` placeholders are bound, never spliced.
// Error code mapping follows the conventions used elsewhere in shield:
//   "connection_lost"      Lost connection / server gone
//   "connection_timeout"   query timed out
//   "syntax_error"         SQL parse / grammar error
//   "constraint_violation" Duplicate key / FK / CHECK
//   "transaction_aborted"  Deadlock
//   "db_query_failed"      Catch-all for other errors
//
// Lua autonomy: register_lua installs the shared callable namespace
// shield.database.mysql(binding). Each call acquires a connection from a
// per-instance connection pool, runs the statement, and returns the
// connection to the pool on scope exit. Inside a coroutine dispatch the
// query/query_one/execute entry points are async (docs/db-async-design.md):
// the caller suspends and the acquire + statement run on the instance worker
// pool; "async: false" (or any non-coroutine context) keeps the synchronous
// inline behaviour. The C vtable still creates/closes a fresh connection per
// connect — C-ABI callers do NOT get pooling or the async path, only Lua
// callers do. This keeps the vtable semantics unchanged while giving Lua
// scripts the warm-connection performance they expect.

#include <mysql/errmsg.h>
#include <mysql/mysql.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
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
#include "shield_db_mysql_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

int mysql_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new mysql_instance;
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
            return &mysql_pool_stats_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        // shell is the first member of mysql_instance (offset 0), so self
        // points at the enclosing mysql_instance. Standard C-ABI pattern.
        auto* inst = reinterpret_cast<mysql_instance*>(self);
        unregister_instance(inst->instance_id);
        // Close the worker intake, drain in-flight tasks (each bounded by
        // its own acquire/query timeouts), roll back any transaction still
        // holding a connection, and only then drain the pool — the design
        // doc's shutdown ordering.
        {
            std::lock_guard lk(inst->worker_mu);
            inst->worker_stop = true;
        }
        inst->worker_cv.notify_all();
        for (auto& t : inst->workers) {
            if (t.joinable()) t.join();
        }
        if (inst->tx_lane.joinable()) inst->tx_lane.join();
        drain_held_tx(inst);
        drain_pool(inst);
        delete inst;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.mysql",
        "1.0.0",
        mysql_create,
    };
    return &abi;
}
