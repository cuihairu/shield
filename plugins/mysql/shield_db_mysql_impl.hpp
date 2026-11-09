// [SHIELD_PLUGIN] database.mysql — internal cross-TU declarations for the
// shield_db_mysql split.
//
// Not a public contract: the plugin ABI stays in shield/plugin/*.h. The
// data structures below move here verbatim so the shield_db_mysql_*.cpp
// translation units share one definition; the functions are defined flat
// (external linkage) in their own TU, one declaration each.
#pragma once

#include <mysql/mysql.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
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
// Prepared statement engine
//
// Statements run through mysql_stmt_* so `?` placeholders are bound, never
// spliced. Result rows are fetched with per-column typed buffers (chosen from
// the column's wire type) so Lua callers keep their type semantics — an INT
// column comes back as a Lua integer, not "42".
// ---------------------------------------------------------------------------

// Cell kinds for stmt_result::kinds.
enum : unsigned char {
    kCellInt = 'i',
    kCellDbl = 'd',
    kCellStr = 's',
    kCellNull = 'n'
};

struct stmt_result {
    bool has_rows = false;
    int col_count = 0;
    int row_count = 0;
    std::vector<std::string> col_names;
    // Per [row][col] typed cells; exactly one of the three is live per cell,
    // selected by kinds[row][col].
    std::vector<std::vector<int64_t>> ints;
    std::vector<std::vector<double>> dbls;
    std::vector<std::vector<std::string>> strs;
    std::vector<std::vector<unsigned char>> kinds;
    int64_t affected = 0;
    int64_t insert_id = 0;
};

struct stmt_error {
    std::string code;
    std::string msg;
};

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json) and a per-instance Session pool. It registers itself in a
// process-wide map so the Lua callable namespace can resolve plugins.bindings
// logical names to instances. The C++ vtable is also served through
// get_interface() for any C-ABI consumer that prefers the raw connect/query
// surface.
// ---------------------------------------------------------------------------

// One queued async SQL task: submitted on the actor thread by the proxy's
// __db_submit closure (after the caller coroutine suspended), executed on the
// instance's worker pool, completed via host_api->lua_resume_session.
// What a worker task asks for (docs/db-async-design.md M3/M4).
enum class task_kind {
    stmt,         // pool-level query/query_one/execute (acquire on worker)
    tx_begin,     // acquire + START TRANSACTION; resume carries the tx token
    tx_stmt,      // query/query_one/execute on the connection tx_token holds
    tx_commit,    // COMMIT on the held connection, then release
    tx_rollback,  // ROLLBACK on the held connection, then release
};

struct pool_guard;  // owns a checked-out pool connection (defined below)

struct mysql_task {
    uint64_t session = 0;
    task_kind kind = task_kind::stmt;
    uint64_t tx_token = 0;  // tx_stmt / tx_commit / tx_rollback target
    std::string method;     // run mode for stmt and tx_stmt
    std::string sql;
    nlohmann::json params = nlohmann::json::array();
};

struct mysql_instance {
    shield_plugin_instance_v1 shell;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string instance_id;

    // Parsed config (from args->config_json). Defaults match manifest.yaml.
    std::string host = "127.0.0.1";
    int port = 3306;  // classic MySQL client/server protocol port
    std::string database;
    std::string username;
    std::string password;
    int connect_timeout_ms = 5000;
    int query_timeout_ms = 5000;
    int pool_size = 4;
    int acquire_timeout_ms = 10000;  // how long to wait when the pool is full
    // Async path (docs/db-async-design.md M3): inside a coroutine dispatch the
    // proxy methods suspend the caller and the SQL runs on the worker pool
    // below; "async: false" (and every non-coroutine context) keeps the
    // original inline synchronous behavior.
    bool async_enabled = true;  // from config "async"
    int call_timeout_ms = 0;    // from config "call_timeout_ms"; 0 ->
                                // query_timeout_ms + 500 (design doc default)

    // Pool state — protected by pool_mu.
    std::mutex pool_mu;
    std::condition_variable pool_cv;
    std::queue<MYSQL*> free_list;
    int current_size = 0;  // live connections (in free_list + checked out)

    // Worker pool (M3): acquire_session runs ON a worker, so a pool exhausted
    // by concurrent holders suspends callers instead of blocking the actor
    // thread. Sized to the connection pool (capped), started lazily on the
    // first async submit. shutdown() closes the intake and joins the workers
    // BEFORE the connection pool is drained.
    std::mutex worker_mu;
    std::condition_variable worker_cv;
    std::deque<mysql_task> worker_tasks;
    bool worker_stop = false;
    bool worker_started = false;
    std::vector<std::thread> workers;

    // Open async transactions (M4): connections held across suspended tx
    // bodies, keyed by the token the shim puts on every tx task. Token-bound
    // tasks run on the single tx_lane thread so per-connection use stays
    // serialized — a caller timeout queues its rollback behind a
    // still-running statement instead of racing it on a second worker.
    // Guarded by worker_mu; guards are extracted (lock dropped) before any
    // libmysql call, and their destructor is what releases the connection.
    std::unordered_map<uint64_t, std::unique_ptr<pool_guard>> held_tx;
    std::deque<mysql_task> tx_tasks;
    std::thread tx_lane;
    bool tx_lane_started = false;
    std::atomic<uint64_t> next_tx_token{1};

    // Observability (M4): in-flight async calls; shield_pool_stats.holding
    // reads held_tx.size() under worker_mu.
    std::atomic<int> pending_async{0};
};

// RAII guard — releases the connection back to the pool on destruction. If
// the connection is marked broken (e.g. the caller observed a connection
// error), the guard drops it instead of returning a known-bad connection.
struct pool_guard {
    mysql_instance* inst = nullptr;
    MYSQL* sess = nullptr;
    bool broken = false;

    pool_guard() = default;
    pool_guard(mysql_instance* i, MYSQL* s) : inst(i), sess(s) {}

    ~pool_guard() {
        if (!inst || !sess) return;
        if (broken) {
            // Discard: close (best effort), then decrement live count.
            mysql_close(sess);
            std::lock_guard lk(inst->pool_mu);
            inst->current_size -= 1;
            inst->pool_cv.notify_one();
            return;
        }
        std::lock_guard lk(inst->pool_mu);
        inst->free_list.push(sess);
        inst->pool_cv.notify_one();
    }

    pool_guard(const pool_guard&) = delete;
    pool_guard& operator=(const pool_guard&) = delete;
    pool_guard(pool_guard&& o) noexcept
        : inst(o.inst), sess(o.sess), broken(o.broken) {
        o.inst = nullptr;
        o.sess = nullptr;
    }
    pool_guard& operator=(pool_guard&& o) noexcept {
        if (this != &o) {
            inst = o.inst;
            sess = o.sess;
            broken = o.broken;
            o.inst = nullptr;
            o.sess = nullptr;
        }
        return *this;
    }

    MYSQL* operator->() const { return sess; }
    MYSQL& operator*() const { return *sess; }
    explicit operator bool() const { return sess != nullptr; }
};

// Defined in shield_db_mysql_exec.cpp: prepared-statement executor and the
// C-ABI vtable factory.
const char* map_mysql_error(unsigned err, const char* msg);
void clear_result(shield_db_result* r);
int fill_mysql_error(MYSQL* mysql, shield_db_result* out);
int fill_text_error(const char* msg, shield_db_result* out);
bool col_is_int(const MYSQL_FIELD& f);
bool col_is_double(const MYSQL_FIELD& f);
bool exec_typed(MYSQL* mysql, const char* sql,
                const std::vector<MYSQL_BIND>& params, stmt_result* res,
                stmt_error* err);
int run_stmt(MYSQL* mysql, const char* sql, const char* const* params,
             int n_params, bool collect_rows, shield_db_result* out);
int run_simple(MYSQL* mysql, const char* sql, shield_db_result* out);
const shield_database_v1& db_vtable();

// Defined in shield_db_mysql_async.cpp: instance registry, pool checkout,
// the async worker engine and the pool-stats vtable.
std::mutex& instances_mu();
std::map<std::string, mysql_instance*>& instances_map();
void register_instance(mysql_instance* inst);
void unregister_instance(const std::string& id);
mysql_instance* find_instance(const std::string& id);
void parse_instance_config(mysql_instance* inst, const char* config_json);
std::unique_ptr<pool_guard> acquire_session(mysql_instance* inst,
                                            std::string* err);
void drain_pool(mysql_instance* inst);
int caller_timeout_ms(const mysql_instance* inst);
bool resume_caller(mysql_instance* inst, uint64_t session, bool ok,
                   const nlohmann::json& payload);
void warn_rejected(mysql_instance* inst, uint64_t session, const char* what);
void mark_broken_on_conn_error(const std::string& code, pool_guard* guard);
void run_task(mysql_instance* inst, mysql_task task);
void task_loop(mysql_instance* inst, bool tx_lane);
int worker_count(const mysql_instance* inst);
void submit_async(mysql_instance* inst, mysql_task task);
void drain_held_tx(mysql_instance* inst);
bool run_tx_sql(MYSQL* mysql, const char* sql, const char** code,
                std::string* msg);
const shield_pool_stats_v1& mysql_pool_stats_vtable();

// Defined in shield_db_mysql_lua.cpp: Lua facade entry points.
nlohmann::json row_to_json(const stmt_result& res, int r);
bool run_statement_json(MYSQL* mysql, const std::string& sql,
                        const nlohmann::json& params, const std::string& mode,
                        nlohmann::json* out, std::string* err_code,
                        std::string* err_msg);
int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err);
