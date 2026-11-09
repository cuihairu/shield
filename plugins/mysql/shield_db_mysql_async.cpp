// [SHIELD_PLUGIN] database.mysql — instance registry, pool checkout
// and the async worker engine (task lanes, tx token lifecycle), the
// synchronous transaction runner and the pool-stats vtable.
// Split out of shield_db_mysql.cpp.
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

// Process-wide registry: instance_id -> mysql_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Read on every proxy creation, so it must be
// thread-safe.
// Intentionally leaked (never destructed): PluginHost's own static destructor
// can run instance shutdown() at process exit, and by then function-local
// statics initialized after the host — these — would already be gone.
// unregister_instance() walking a destroyed map is a use-after-free.
std::mutex& instances_mu() {
    static std::mutex& m = *new std::mutex;
    return m;
}

std::map<std::string, mysql_instance*>& instances_map() {
    static std::map<std::string, mysql_instance*>& m =
        *new std::map<std::string, mysql_instance*>;
    return m;
}

void register_instance(mysql_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}

void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}

mysql_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract the known keys and fall
// back to defaults for anything missing.
void parse_instance_config(mysql_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string())
            inst->host = j["host"].get<std::string>();
        if (j.contains("port") && j["port"].is_number_integer())
            inst->port = j["port"].get<int>();
        if (j.contains("database") && j["database"].is_string())
            inst->database = j["database"].get<std::string>();
        if (j.contains("username") && j["username"].is_string())
            inst->username = j["username"].get<std::string>();
        if (j.contains("user") && j["user"].is_string())
            inst->username = j["user"].get<std::string>();  // alias
        if (j.contains("password") && j["password"].is_string())
            inst->password = j["password"].get<std::string>();
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer())
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        if (j.contains("query_timeout_ms") &&
            j["query_timeout_ms"].is_number_integer())
            inst->query_timeout_ms = j["query_timeout_ms"].get<int>();
        if (j.contains("pool_size") && j["pool_size"].is_number_integer())
            inst->pool_size = j["pool_size"].get<int>();
        if (j.contains("acquire_timeout_ms") &&
            j["acquire_timeout_ms"].is_number_integer())
            inst->acquire_timeout_ms = j["acquire_timeout_ms"].get<int>();
        if (j.contains("async") && j["async"].is_boolean())
            inst->async_enabled = j["async"].get<bool>();
        if (j.contains("call_timeout_ms") &&
            j["call_timeout_ms"].is_number_integer()) {
            inst->call_timeout_ms = j["call_timeout_ms"].get<int>();
            // Startup validation (design doc): an explicit caller budget at
            // or above the driver budget inverts the discipline's
            // call_timeout < query_timeout — the business-side shield.call
            // expires first and the suspend completion lands with nobody
            // waiting. Warn, not reject — the sync path lives under the
            // same pair.
            if (inst->call_timeout_ms > 0 &&
                inst->call_timeout_ms >= inst->query_timeout_ms &&
                inst->host_api != nullptr && inst->host_api->log != nullptr) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "call_timeout_ms (%d) >= query_timeout_ms (%d): "
                              "caller budgets outlive their driver windows",
                              inst->call_timeout_ms, inst->query_timeout_ms);
                inst->host_api->log(SHIELD_LOG_WARN, "database.mysql",
                                    inst->instance_id.c_str(), buf);
            }
        }
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

namespace {
// ---------------------------------------------------------------------------
// Connection pool
//
// A TCP + auth handshake is expensive, so the Lua proxy keeps a per-instance
// free list of live MYSQL* connections. acquire_session() returns a RAII
// guard that pushes the connection back onto the free list (and notifies one
// waiter) when it goes out of scope.
//
// Lifecycle:
//   - Try free_list first (fast path, no construction).
//   - If free_list is empty and current_size < pool_size, open a new
//     connection (current_size is bumped under the lock so concurrent
//     openers don't overshoot).
//   - Otherwise wait on pool_cv up to acquire_timeout_ms, then fail.
//
// Broken connections are discarded (not pushed back) so the next acquirer
// gets a fresh one. current_size is decremented when a connection is dropped.
// ---------------------------------------------------------------------------

// Open a new connection from the instance config. Returns nullptr on failure
// and fills *err with the server's message.
MYSQL* open_conn(const mysql_instance* inst, std::string* err) {
    MYSQL* db = mysql_init(nullptr);
    if (!db) {
        if (err) *err = "mysql: out of memory";
        return nullptr;
    }
    unsigned int connect_timeout = static_cast<unsigned int>(
        inst->connect_timeout_ms > 0 ? (inst->connect_timeout_ms + 999) / 1000
                                     : 5);
    // Approximate the per-statement query timeout with socket read/write
    // timeouts (the client library has no per-statement deadline).
    unsigned int query_timeout = static_cast<unsigned int>(
        inst->query_timeout_ms > 0 ? (inst->query_timeout_ms + 999) / 1000 : 5);
    mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &query_timeout);
    mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &query_timeout);
    mysql_options(db, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(
            db, inst->host.c_str(),
            inst->username.empty() ? "root" : inst->username.c_str(),
            inst->password.c_str(), inst->database.c_str(),
            inst->port > 0 ? static_cast<unsigned int>(inst->port) : 3306,
            nullptr, 0)) {
        if (err) *err = std::string("mysql connect: ") + mysql_error(db);
        mysql_close(db);
        return nullptr;
    }
    return db;
}
}  // namespace

// Acquire a session from the pool. On success returns a guard whose `sess`
// is non-null. On failure returns a guard with sess == nullptr and fills
// *err (if non-null).
//
// Three outcomes under the lock:
//   - free_list non-empty          -> pop and return (fast path)
//   - current_size < pool_size     -> reserve a slot (current_size++) and open
//                                     a fresh Session OUTSIDE the lock so a
//                                     slow connect doesn't stall other waiters
//   - pool full                    -> wait on pool_cv up to acquire_timeout_ms
//
// Broken sessions (set by run_statement on connection-lost errors) are
// discarded by the guard's destructor and current_size is decremented so the
// next acquirer can open a replacement.
std::unique_ptr<pool_guard> acquire_session(mysql_instance* inst,
                                            std::string* err) {
    if (!inst) {
        if (err) *err = "mysql: null instance";
        return std::make_unique<pool_guard>();
    }

    // Outcome enum for the in-lock probe: TAKE (free connection), OPEN (slot
    // reserved, must construct outside the lock), WAIT (pool full).
    enum class Probe { take, open, wait };
    MYSQL* taken = nullptr;
    Probe probe;
    {
        std::lock_guard lk(inst->pool_mu);
        if (!inst->free_list.empty()) {
            taken = inst->free_list.front();
            inst->free_list.pop();
            probe = Probe::take;
        } else if (inst->current_size < inst->pool_size) {
            inst->current_size += 1;  // reserve the slot
            probe = Probe::open;
        } else {
            probe = Probe::wait;
        }
    }

    if (probe == Probe::take) {
        return std::make_unique<pool_guard>(inst, taken);
    }

    if (probe == Probe::open) {
        std::string open_err;
        MYSQL* db = open_conn(inst, &open_err);
        if (db) {
            return std::make_unique<pool_guard>(inst, db);
        }
        if (err) *err = open_err;
        // Open failed — release the slot and wake one waiter.
        std::lock_guard lk(inst->pool_mu);
        inst->current_size -= 1;
        inst->pool_cv.notify_one();
        return std::make_unique<pool_guard>();
    }

    // probe == Probe::wait
    std::unique_lock lk(inst->pool_mu);
    bool got = inst->pool_cv.wait_for(
        lk,
        std::chrono::milliseconds(
            inst->acquire_timeout_ms > 0 ? inst->acquire_timeout_ms : 10000),
        [&] {
            return !inst->free_list.empty() ||
                   inst->current_size < inst->pool_size;
        });
    if (!got) {
        if (err) *err = "mysql: connection pool exhausted (acquire timeout)";
        return std::make_unique<pool_guard>();
    }
    if (!inst->free_list.empty()) {
        MYSQL* s = inst->free_list.front();
        inst->free_list.pop();
        lk.unlock();
        return std::make_unique<pool_guard>(inst, s);
    }
    // Slot opened up — reserve and open outside the lock.
    inst->current_size += 1;
    lk.unlock();
    std::string open_err;
    MYSQL* db = open_conn(inst, &open_err);
    if (db) {
        return std::make_unique<pool_guard>(inst, db);
    }
    if (err) *err = open_err;
    std::lock_guard lk2(inst->pool_mu);
    inst->current_size -= 1;
    inst->pool_cv.notify_one();
    return std::make_unique<pool_guard>();
}

// Drain the pool on shutdown. Assumes no other thread is acquiring (host
// guarantees shutdown is the last call on the instance).
void drain_pool(mysql_instance* inst) {
    std::lock_guard lk(inst->pool_mu);
    while (!inst->free_list.empty()) {
        MYSQL* s = inst->free_list.front();
        inst->free_list.pop();
        mysql_close(s);
    }
    inst->current_size = 0;
}

// ---------------------------------------------------------------------------
// Async entry (docs/db-async-design.md M3).
//
// Inside a coroutine dispatch query/query_one/execute suspend the caller via
// the host's lua_suspend_current primitive and the SQL — including the pool
// acquire — runs on the instance worker pool below. This section never touches
// a lua_State: it speaks JSON in both directions, and the host's resume
// converts the payload back into Lua values.
// ---------------------------------------------------------------------------

// Caller-side suspend budget (design doc: query_timeout_ms + 500ms slack
// unless explicitly overridden).
int caller_timeout_ms(const mysql_instance* inst) {
    return inst->call_timeout_ms > 0 ? inst->call_timeout_ms
                                     : inst->query_timeout_ms + 500;
}

bool run_tx_sql(MYSQL* mysql, const char* sql, const char** code,
                std::string* msg);  // defined with the sync transaction below

// Deliver one completion. Returns true when the suspended caller claimed it.

bool resume_caller(mysql_instance* inst, uint64_t session, bool ok,
                   const nlohmann::json& payload) {
    if (inst->host_api == nullptr ||
        inst->host_api->lua_resume_session == nullptr) {
        return false;
    }
    const std::string str = payload.dump();
    return inst->host_api->lua_resume_session(inst->ctx, session, ok ? 1 : 0,
                                              str.c_str()) == 0;
}

void warn_rejected(mysql_instance* inst, uint64_t session, const char* what) {
    if (inst->host_api == nullptr || inst->host_api->log == nullptr) return;
    std::string msg = std::string("async ") + what + " completion rejected " +
                      "for session " + std::to_string(session) +
                      " (caller timeout or service gone)";
    inst->host_api->log(SHIELD_LOG_WARN, "database.mysql",
                        inst->instance_id.c_str(), msg.c_str());
}

// Poison the connection a just-failed statement ran on (same codes the sync
// path marks broken).
void mark_broken_on_conn_error(const std::string& code, pool_guard* guard) {
    if (code == "connection_lost" || code == "connection_timeout") {
        guard->broken = true;
    }
}

// Execute one task off the actor thread. THE M3 POINT — the pool acquire for
// plain statements and BEGIN runs here, so a pool exhausted by concurrent
// holders suspends the calling coroutine instead of blocking the actor. M4
// adds the token-bound tx forms running on the held connection.
void run_task(mysql_instance* inst, mysql_task task) {
    bool ok = false;
    nlohmann::json result;  // success payload value
    nlohmann::json err;     // {code, message} on failure
    std::unique_ptr<pool_guard> guard;
    uint64_t new_token = 0;  // set by a successful tx_begin

    if (task.kind == task_kind::stmt) {
        std::string acquire_err;
        guard = acquire_session(inst, &acquire_err);
        if (guard && *guard) {
            std::string err_code;
            std::string err_msg;
            ok = run_statement_json(guard->sess, task.sql, task.params,
                                    task.method, &result, &err_code, &err_msg);
            if (!ok) {
                mark_broken_on_conn_error(err_code, guard.get());
                err = {{"code", err_code}, {"message", err_msg}};
            }
        } else {
            err = {{"code", "connection_failed"},
                   {"message", acquire_err.empty()
                                   ? std::string("pool acquire failed")
                                   : acquire_err}};
        }
    } else if (task.kind == task_kind::tx_begin) {
        std::string acquire_err;
        guard = acquire_session(inst, &acquire_err);
        if (!(guard && *guard)) {
            err = {{"code", "connection_failed"},
                   {"message", acquire_err.empty()
                                   ? std::string("pool acquire failed")
                                   : acquire_err}};
        } else {
            const char* code = nullptr;
            std::string msg;
            if (run_tx_sql(guard->sess, "START TRANSACTION", &code, &msg)) {
                new_token = inst->next_tx_token.fetch_add(1);
                {
                    std::lock_guard lk(inst->worker_mu);
                    inst->held_tx[new_token] = std::move(guard);
                }
                ok = true;
                result = new_token;  // the shim carries this on every tx task
            } else {
                guard->broken = true;
                err = {{"code", code ? code : "connection_failed"},
                       {"message", msg}};
            }
        }
    } else {
        // Token-bound: statement / COMMIT / ROLLBACK on a held connection.
        {
            std::lock_guard lk(inst->worker_mu);
            auto it = inst->held_tx.find(task.tx_token);
            if (it != inst->held_tx.end()) guard = std::move(it->second);
            // End-of-tx tasks always take the token out: whatever happens,
            // the connection is released below.
            if (task.kind != task_kind::tx_stmt)
                inst->held_tx.erase(task.tx_token);
        }
        if (!(guard && *guard)) {
            err = {{"code", "transaction_closed"},
                   {"message", "transaction is no longer open"}};
        } else if (task.kind == task_kind::tx_stmt) {
            std::string err_code;
            std::string err_msg;
            ok = run_statement_json(guard->sess, task.sql, task.params,
                                    task.method, &result, &err_code, &err_msg);
            if (!ok) {
                mark_broken_on_conn_error(err_code, guard.get());
                err = {{"code", err_code}, {"message", err_msg}};
            }
            // Hand the connection back BEFORE resuming: the caller's next
            // statement — or the rollback a caller timeout triggers — must
            // find it (tx-lane serialization keeps users one at a time).
            {
                std::lock_guard lk(inst->worker_mu);
                inst->held_tx[task.tx_token] = std::move(guard);
            }
        } else {
            const char* code = nullptr;
            std::string msg;
            const char* sql =
                task.kind == task_kind::tx_commit ? "COMMIT" : "ROLLBACK";
            if (run_tx_sql(guard->sess, sql, &code, &msg)) {
                ok = true;
                result = true;
            } else {
                guard->broken = true;
                err = {{"code", code ? code : "connection_failed"},
                       {"message", msg}};
            }
            // guard's destructor releases: recycled when healthy, closed
            // when broken.
        }
    }

    // Payload = the VALUES array only — resume_suspended_caller pushes the
    // ok boolean itself, so the shim sees (ok, value...) from [value] and
    // (false, err_table) from [{code, message}].
    const nlohmann::json payload =
        ok ? nlohmann::json::array({result}) : nlohmann::json::array({err});
    if (!resume_caller(inst, task.session, ok, payload)) {
        if (task.kind == task_kind::stmt) {
            // Discarded statement result — poison so it is never reused.
            if (guard && *guard) guard->broken = true;
        } else if (task.kind == task_kind::tx_begin && new_token != 0) {
            // Nobody can commit or roll back a tx whose caller is gone: take
            // it back and end it. ROLLBACK is deterministic here, so the
            // connection recycles (poisoned only if the ROLLBACK fails).
            std::unique_ptr<pool_guard> g;
            {
                std::lock_guard lk(inst->worker_mu);
                auto it = inst->held_tx.find(new_token);
                if (it != inst->held_tx.end()) {
                    g = std::move(it->second);
                    inst->held_tx.erase(it);
                }
            }
            if (g && *g) {
                const char* code = nullptr;
                std::string msg;
                if (!run_tx_sql(g->sess, "ROLLBACK", &code, &msg)) {
                    g->broken = true;
                }
            }
        }
        // tx_stmt / tx_commit / tx_rollback rejections need no extra work:
        // the shim's rollback (queued behind tx_stmt on the tx lane) or the
        // shutdown drain owns the connection.
        warn_rejected(inst, task.session,
                      task.kind == task_kind::stmt ? "statement" : "tx");
    }
    inst->pending_async.fetch_sub(1, std::memory_order_relaxed);
}

// Worker body: drain one queue until stopped. tx_lane == true serves the
// serialized transaction queue; general workers serve pool statements and
// BEGIN (BEGIN stays off the tx lane so a pool-exhausted acquire can never
// block another transaction's commit behind it).
void task_loop(mysql_instance* inst, bool tx_lane) {
    std::unique_lock lk(inst->worker_mu);
    while (true) {
        inst->worker_cv.wait(lk, [&] {
            return inst->worker_stop || (tx_lane ? !inst->tx_tasks.empty()
                                                 : !inst->worker_tasks.empty());
        });
        std::deque<mysql_task>& q =
            tx_lane ? inst->tx_tasks : inst->worker_tasks;
        if (q.empty()) {
            if (inst->worker_stop) return;
            continue;
        }
        mysql_task task = std::move(q.front());
        q.pop_front();
        lk.unlock();
        run_task(inst, std::move(task));
        lk.lock();
    }
}

// Worker pool size: never more than the connection pool (extra workers would
// only queue on acquire), hard-capped so thread counts stay sane.
int worker_count(const mysql_instance* inst) {
    int n = inst->pool_size > 0 ? inst->pool_size : 1;
    return std::min(n, 8);
}

// Submit one task: lazily start the worker pool (and the tx lane for
// token-bound tasks), enqueue, wake. Runs on the actor thread right after the
// caller suspended — the task may therefore complete before the shim reaches
// coroutine.yield(); the host's yield-window guard requeues that completion
// over the actor mailbox.
void submit_async(mysql_instance* inst, mysql_task task) {
    {
        std::lock_guard lk(inst->worker_mu);
        if (!inst->worker_started) {
            inst->worker_started = true;
            int n = worker_count(inst);
            inst->workers.reserve(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                inst->workers.emplace_back(task_loop, inst, false);
            }
        }
        const bool lane =
            task.kind != task_kind::stmt && task.kind != task_kind::tx_begin;
        if (lane && !inst->tx_lane_started) {
            inst->tx_lane_started = true;
            inst->tx_lane = std::thread(task_loop, inst, true);
        }
        if (lane) {
            inst->tx_tasks.push_back(std::move(task));
        } else {
            inst->worker_tasks.push_back(std::move(task));
        }
        inst->pending_async.fetch_add(1, std::memory_order_relaxed);
    }
    inst->worker_cv.notify_all();
}

// Roll back and release every still-held transaction connection (shutdown
// path: after the workers are joined nothing can end them on their own).
void drain_held_tx(mysql_instance* inst) {
    std::vector<std::unique_ptr<pool_guard>> guards;
    {
        std::lock_guard lk(inst->worker_mu);
        guards.reserve(inst->held_tx.size());
        for (auto& [tok, g] : inst->held_tx) guards.push_back(std::move(g));
        inst->held_tx.clear();
    }
    for (auto& g : guards) {
        if (g && *g) {
            const char* code = nullptr;
            std::string msg;
            if (!run_tx_sql(g->sess, "ROLLBACK", &code, &msg)) g->broken = true;
        }
        g.reset();  // release: recycled when healthy, closed when broken
    }
}

namespace {
// shield.pool.stats.v1 — real gauges from the per-instance pool. The driver
// tracks capacity/usage but no cumulative counters, so those stay -1
// (unknown), matching the ABI's sentinel semantics.
int mysql_pool_get_stats(struct shield_plugin_instance_v1* self,
                         struct shield_pool_stats* out) {
    auto* inst = reinterpret_cast<mysql_instance*>(self);
    if (!inst || !out) return -1;
    {
        std::lock_guard lk(inst->pool_mu);
        int idle = static_cast<int>(inst->free_list.size());
        out->max_size = inst->pool_size;
        out->size = inst->current_size;
        out->idle = idle;
        out->in_use = inst->current_size - idle;
        out->waiters = -1;
        out->acquire_timeout_total = -1;
        out->acquire_total = -1;
        out->create_total = -1;
        out->destroy_total = -1;
        out->eviction_total = -1;
        out->health_check_failures_total = -1;
        out->last_error_epoch_ms = -1;
    }
    // Locks are taken sequentially, never nested (workers hold worker_mu
    // only for map moves, never across pool_mu).
    {
        std::lock_guard lk(inst->worker_mu);
        out->holding = static_cast<int>(inst->held_tx.size());
    }
    out->pending_async = inst->pending_async.load();
    return 0;
}
}  // namespace

const shield_pool_stats_v1& mysql_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &mysql_pool_get_stats,
    };
    return v;
}

// Execute a transaction-control statement (BEGIN/COMMIT/ROLLBACK) on a
// pooled connection. On failure fills *code/*msg with the mapped error.
bool run_tx_sql(MYSQL* mysql, const char* sql, const char** code,
                std::string* msg) {
    if (mysql_real_query(mysql, sql,
                         static_cast<unsigned long>(std::strlen(sql))) == 0) {
        return true;
    }
    *code = map_mysql_error(mysql_errno(mysql), mysql_error(mysql));
    *msg = std::string(sql) + ": " + mysql_error(mysql);
    return false;
}
