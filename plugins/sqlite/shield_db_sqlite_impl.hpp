// [SHIELD_PLUGIN] database.sqlite — internal cross-TU declarations
// for the sqlite driver split. Not a public contract: the exported
// ABI stays in shield/plugin/*.h. The task and instance structs move
// here verbatim so the shield_db_sqlite*.cpp translation units share
// one instance layout; the file-local helpers shared across the split
// are defined flat (external linkage) in their own TU, one declaration
// each.
#pragma once

#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>

#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"

// One queued async SQL task: submitted on the actor thread by the proxy's
// __db_submit closure (after the caller coroutine suspended), executed on the
// instance's worker thread, completed via host_api->lua_resume_session.
// What a worker task asks for (docs/db-async-design.md M2/M4).
enum class task_kind {
    stmt,         // pool-level query/query_one/execute (per-call open)
    tx_begin,     // open + BEGIN; resume carries the tx token
    tx_stmt,      // query/query_one/execute on the connection tx_token holds
    tx_commit,    // COMMIT on the held connection, then close
    tx_rollback,  // ROLLBACK on the held connection, then close
};

struct sqlite_task {
    uint64_t session = 0;
    task_kind kind = task_kind::stmt;
    uint64_t tx_token = 0;  // tx_stmt / tx_commit / tx_rollback target
    std::string method;     // run mode for stmt and tx_stmt
    std::string sql;
    nlohmann::json params = nlohmann::json::array();
};

struct sqlite_instance {
    shield_plugin_instance_v1 shell;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string instance_id;
    std::string database_path = ":memory:";  // from config "database"
    int query_timeout_ms = 5000;             // from config "query_timeout_ms"
    // Async path (docs/db-async-design.md M2): inside a coroutine dispatch the
    // proxy methods suspend the caller and the SQL runs on the worker thread
    // below; "async: false" (and every non-coroutine context) keeps the
    // original inline synchronous behavior.
    bool async_enabled = true;  // from config "async"
    int call_timeout_ms = 0;    // from config "call_timeout_ms"; 0 ->
                                // query_timeout_ms + 500 (design doc default)
    // Single worker thread, started lazily on the first async submit. The
    // task queue + stop flag live under worker_mu; shutdown() closes the
    // queue and joins the thread BEFORE the instance is deleted. The single
    // thread is its own serialization lane: a caller timeout queues its
    // rollback behind a still-running statement, never beside it.
    std::mutex worker_mu;
    std::condition_variable worker_cv;
    std::deque<sqlite_task> worker_tasks;
    bool worker_stop = false;
    bool worker_started = false;
    std::thread worker_thread;

    // Open async transactions (M4): sqlite3* handles held across suspended
    // tx bodies, keyed by the token the shim puts on every tx task. Guarded
    // by worker_mu; the held handle is used only by the single worker and
    // closed when the transaction ends (or on the shutdown drain).
    std::unordered_map<uint64_t, sqlite3*> held_tx;
    std::atomic<uint64_t> next_tx_token{1};

    // Observability (M4): in-flight async calls; shield_pool_stats.holding
    // reads held_tx.size() under worker_mu. sqlite has no pool, so the pool
    // gauges stay -1 (not applicable).
    std::atomic<int> pending_async{0};
};

// Defined in shield_db_sqlite_exec.cpp: stable error-code mapping
// shared by the executor, the JSON runner and the Lua facade.
const char* map_sqlite_error(int rc);

// Defined in shield_db_sqlite_exec.cpp: the legacy SQL surface, served
// through get_interface() in sqlite_create.
const shield_database_v1& db_vtable();

// Defined in shield_db_sqlite_async.cpp: the process-wide registry.
void register_instance(sqlite_instance* inst);
void unregister_instance(const std::string& id);
sqlite_instance* find_instance(const std::string& id);

// Defined in shield_db_sqlite_async.cpp: config extraction used by
// sqlite_create.
void parse_instance_config(sqlite_instance* inst, const char* config_json);

// Defined in shield_db_sqlite_async.cpp: caller-side suspend budget
// consumed by the async shim.
int caller_timeout_ms(const sqlite_instance* inst);

// Defined in shield_db_sqlite_async.cpp: one fresh per-call connection
// (the engine's runner and the Lua facade's sync path both use it).
sqlite3* open_connection(const sqlite_instance* inst, std::string* err_msg);

// Defined in shield_db_sqlite_async.cpp: shutdown-side drain used by
// the shell shutdown callback.
void drain_held_tx(sqlite_instance* inst);

// Defined in shield_db_sqlite_async.cpp: worker submit used by the
// instance proxy's async branch.
void submit_async(sqlite_instance* inst, sqlite_task task);

// Defined in shield_db_sqlite_lua.cpp: register_lua implementation,
// installed by sqlite_create on the shell.
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);

// Defined in shield_db_sqlite_async.cpp: the pool-stats surface, served
// through get_interface() in sqlite_create.
const shield_pool_stats_v1& sqlite_pool_stats_vtable();
