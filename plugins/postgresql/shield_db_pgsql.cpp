// [SHIELD_PLUGIN] database.postgresql — PostgreSQL provider for
// shield.database.v1 (v1 ABI). Plugin entry: instance lifecycle
// (pg_create) and the get_v1 export. The C-ABI executor, async
// worker engine and Lua facade live in the sibling translation
// units.
//
// v1 ABI (shield_plugin_get_v1). libpq is a fairly thin layer over the wire
// protocol; the interesting work here is the per-instance connection pool.
//
// Two implementation notes worth calling out:
//
// 1. Placeholder syntax. shield's other plugins use ? for parameters
//    (MySQL/SQLite style). libpq expects $1, $2, ... so we rewrite
//    the SQL on the fly. The translator is naive about string literals
//    and comments — it counts ? anywhere. Production queries that put
//    ? inside SQL string literals will break. The shield::data SQL
//    helpers never emit such SQL, so this is acceptable for v1.
//
// 2. Transactions. PostgreSQL supports SAVEPOINT, but we only expose
//    BEGIN/COMMIT/ROLLBACK to match the shield_database_v1 ABI. The
//    pool serialises connection handoff so nested transactional use
//    is the caller's responsibility.
//
// Connection pool: each pgsql_instance owns a free-list of PGconn* guarded
// by a mutex + condition variable. acquire_conn() pops one or grows the
// pool up to `pool_size`; release (via pool_guard RAII) pushes the conn
// back and notifies one waiter. On instance shutdown the pool is drained
// and every PGconn is PQfinish()'d. If a query detects CONNECTION_BAD
// the conn is PQfinish()'d on release instead of being returned to the
// free-list, so the next acquire grows a fresh connection.
//
// Lua async entry (docs/db-async-design.md M3): inside a coroutine dispatch
// query/query_one/execute suspend the caller and the acquire + statement run
// on the instance worker pool; "async: false" (or any non-coroutine context)
// keeps the synchronous inline behaviour below.

#include <libpq-fe.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
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
#include "shield_db_pgsql_impl.hpp"
#include "shield_db_rewrite.hpp"
#include "shield_lua_plugin_binding.hpp"

int pg_create(const shield_plugin_create_args_v1* args,
              shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new pgsql_instance;
    inst->host_api = args ? args->host_api : nullptr;
    inst->ctx = args ? args->ctx : nullptr;
    inst->instance_id = (args && args->instance_id) ? args->instance_id : "";
    parse_instance_config(inst, args ? args->config_json : nullptr);
    if (inst->pool_size < 1) inst->pool_size = 4;
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_DATABASE_INTERFACE) == 0)
            return &db_vtable();
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0)
            return &pg_pool_stats_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        // shell is the first member of pgsql_instance (offset 0), so self
        // points at the enclosing pgsql_instance. Standard C-ABI pattern.
        auto* inst = reinterpret_cast<pgsql_instance*>(self);
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
        "database.postgresql",
        "1.0.0",
        pg_create,
    };
    return &abi;
}
