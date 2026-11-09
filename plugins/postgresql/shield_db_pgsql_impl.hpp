// [SHIELD_PLUGIN] database.postgresql — internal cross-TU declarations for
// the shield_db_pgsql split.
//
// Not a public contract: the plugin ABI stays in shield/plugin/*.h. The
// data structures below move here verbatim so the shield_db_pgsql_*.cpp
// translation units share one definition; the functions are defined flat
// (external linkage) in their own TU, one declaration each.
#pragma once

#include <libpq-fe.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json), owns a per-instance connection pool, and registers itself in
// a process-wide map so the Lua callable namespace can resolve plugins.bindings
// logical names to instances. The C++ vtable is also served through
// get_interface() for any C-ABI consumer that prefers the raw connect/query
// surface.
// ---------------------------------------------------------------------------

// One queued async SQL task: submitted on the actor thread by the proxy's
// __db_submit closure (after the caller coroutine suspended), executed on the
// instance's worker pool, completed via host_api->lua_resume_session.
struct pool_guard;  // owns a checked-out pool connection (defined below)

// What a worker task asks for (docs/db-async-design.md M3/M4).
enum class pg_task_kind {
    stmt,         // pool-level query/query_one/execute (acquire on worker)
    tx_begin,     // acquire + BEGIN; resume carries the tx token
    tx_stmt,      // query/query_one/execute on the connection tx_token holds
    tx_commit,    // COMMIT on the held connection, then release
    tx_rollback,  // ROLLBACK on the held connection, then release
};

struct pg_task {
    uint64_t session = 0;
    pg_task_kind kind = pg_task_kind::stmt;
    uint64_t tx_token = 0;  // tx_stmt / tx_commit / tx_rollback target
    std::string method;     // run mode for stmt and tx_stmt
    std::string sql;
    nlohmann::json params = nlohmann::json::array();
};

struct pgsql_instance {
    shield_plugin_instance_v1 shell;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string instance_id;
    // Parsed config (mirrors shield_db_connect_args keys).
    std::string host = "127.0.0.1";
    int port = 5432;
    std::string database;
    std::string username;
    std::string password;
    int connect_timeout_ms = 5000;
    int query_timeout_ms = 5000;
    int pool_size = 4;
    // Async path (docs/db-async-design.md M3): inside a coroutine dispatch
    // the proxy methods suspend the caller and the SQL runs on the worker
    // pool below; "async: false" (and every non-coroutine context) keeps the
    // original inline synchronous behavior.
    bool async_enabled = true;  // from config "async"
    int call_timeout_ms = 0;    // from config "call_timeout_ms"; 0 ->
                                // query_timeout_ms + 500 (design doc default)

    // Pool state. free_list holds healthy PGconn* ready for reuse.
    // current_size counts every connection we've ever handed out and not
    // yet PQfinish()'d (whether currently in free_list or checked out).
    std::mutex pool_mu;
    std::condition_variable pool_cv;
    std::queue<PGconn*> free_list;
    int current_size = 0;

    // Worker pool (M3): acquire_conn runs ON a worker, so a pool exhausted
    // by concurrent holders suspends callers instead of blocking the actor
    // thread. Sized to the connection pool (capped), started lazily on the
    // first async submit. shutdown() closes the intake and joins the workers
    // BEFORE the connection pool is drained.
    std::mutex worker_mu;
    std::condition_variable worker_cv;
    std::deque<pg_task> worker_tasks;
    bool worker_stop = false;
    bool worker_started = false;
    std::vector<std::thread> workers;

    // Open async transactions (M4): connections held across suspended tx
    // bodies, keyed by the token the shim puts on every tx task. Token-bound
    // tasks run on the single tx_lane thread so per-connection use stays
    // serialized — a caller timeout queues its rollback behind a
    // still-running statement instead of racing it on a second worker.
    // Guarded by worker_mu; guards are extracted (lock dropped) before any
    // libpq call, and their destructor is what releases the connection.
    std::unordered_map<uint64_t, std::unique_ptr<pool_guard>> held_tx;
    std::deque<pg_task> tx_tasks;
    std::thread tx_lane;
    bool tx_lane_started = false;
    std::atomic<uint64_t> next_tx_token{1};

    // Observability (M4): in-flight async calls; shield_pool_stats.holding
    // reads held_tx.size() under worker_mu.
    std::atomic<int> pending_async{0};
};

// RAII guard that returns a checked-out PGconn* to its instance's free-list
// on destruction (or PQfinish'es it if the conn went bad). Move-only.
struct pool_guard {
    pgsql_instance* inst = nullptr;
    PGconn* conn = nullptr;
    bool bad = false;  // set by caller if a query saw CONNECTION_BAD

    pool_guard() = default;
    pool_guard(pgsql_instance* i, PGconn* c) : inst(i), conn(c) {}
    pool_guard(const pool_guard&) = delete;
    pool_guard& operator=(const pool_guard&) = delete;
    pool_guard(pool_guard&& o) noexcept
        : inst(o.inst), conn(o.conn), bad(o.bad) {
        o.inst = nullptr;
        o.conn = nullptr;
    }
    pool_guard& operator=(pool_guard&& o) noexcept {
        if (this != &o) {
            release();
            inst = o.inst;
            conn = o.conn;
            bad = o.bad;
            o.inst = nullptr;
            o.conn = nullptr;
        }
        return *this;
    }
    ~pool_guard() { release(); }

    void release() {
        if (!inst || !conn) return;
        if (bad || PQstatus(conn) == CONNECTION_BAD) {
            PQfinish(conn);
            std::lock_guard lk(inst->pool_mu);
            // current_size tracks live conns; we just freed one.
            if (inst->current_size > 0) --inst->current_size;
            // A waiter might now be allowed to grow a new conn.
            inst->pool_cv.notify_one();
        } else {
            std::lock_guard lk(inst->pool_mu);
            inst->free_list.push(conn);
            inst->pool_cv.notify_one();
        }
        inst = nullptr;
        conn = nullptr;
    }

    PGconn* get() const { return conn; }
};

// Defined in shield_db_pgsql_exec.cpp: C-ABI executor and vtable glue.
const char* map_sqlstate(const char* sqlstate);
const shield_database_v1& db_vtable();

// Defined in shield_db_pgsql_async.cpp: instance registry, pool checkout,
// the async worker engine and the pool-stats vtable.
void register_instance(pgsql_instance* inst);
void unregister_instance(const std::string& id);
pgsql_instance* find_instance(const std::string& id);
void parse_instance_config(pgsql_instance* inst, const char* config_json);
pool_guard acquire_conn(pgsql_instance* inst, std::string* err);
int caller_timeout_ms(const pgsql_instance* inst);
void submit_async(pgsql_instance* inst, pg_task task);
void drain_pool(pgsql_instance* inst);
void drain_held_tx(pgsql_instance* inst);
const shield_pool_stats_v1& pg_pool_stats_vtable();

// Defined in shield_db_pgsql_lua.cpp: Lua facade installer.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);
