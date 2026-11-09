// [SHIELD_PLUGIN] database.postgresql — instance registry, config
// parsing, connection pool checkout, the async worker engine
// (docs/db-async-design.md M3/M4) and the pool-stats vtable.
// Split out of shield_db_pgsql.cpp.
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
using shield::db::pgsql::rewrite_placeholders;

namespace {
// Process-wide registry: instance_id -> pgsql_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here.
// Intentionally leaked (never destructed): PluginHost's own static destructor
// can run instance shutdown() at process exit, and by then function-local
// statics initialized after the host — these — would already be gone.
// unregister_instance() walking a destroyed map is a use-after-free.
std::mutex& instances_mu() {
    static std::mutex& m = *new std::mutex;
    return m;
}

std::map<std::string, pgsql_instance*>& instances_map() {
    static std::map<std::string, pgsql_instance*>& m =
        *new std::map<std::string, pgsql_instance*>;
    return m;
}
}  // namespace

void register_instance(pgsql_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}

void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}

pgsql_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract known keys and fall back
// to defaults for anything missing.
void parse_instance_config(pgsql_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string()) {
            inst->host = j["host"].get<std::string>();
        }
        if (j.contains("port") && j["port"].is_number_integer()) {
            inst->port = j["port"].get<int>();
        }
        if (j.contains("database") && j["database"].is_string()) {
            inst->database = j["database"].get<std::string>();
        }
        if (j.contains("username") && j["username"].is_string()) {
            inst->username = j["username"].get<std::string>();
        }
        if (j.contains("password") && j["password"].is_string()) {
            inst->password = j["password"].get<std::string>();
        }
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer()) {
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        }
        if (j.contains("query_timeout_ms") &&
            j["query_timeout_ms"].is_number_integer()) {
            inst->query_timeout_ms = j["query_timeout_ms"].get<int>();
        }
        if (j.contains("pool_size") && j["pool_size"].is_number_integer()) {
            inst->pool_size = j["pool_size"].get<int>();
        }
        if (j.contains("async") && j["async"].is_boolean()) {
            inst->async_enabled = j["async"].get<bool>();
        }
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
                inst->host_api->log(SHIELD_LOG_WARN, "database.postgresql",
                                    inst->instance_id.c_str(), buf);
            }
        }
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

namespace {
// Build a libpq conninfo string from an instance config.
std::string build_conninfo(const pgsql_instance* inst) {
    std::string s;
    auto append = [&](const char* key, const std::string& val) {
        if (!val.empty()) {
            if (!s.empty()) s.push_back(' ');
            s += key;
            s.push_back('=');
            s += val;
        }
    };
    append("host", inst->host);
    if (inst->port > 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", inst->port);
        append("port", buf);
    }
    append("dbname", inst->database);
    append("user", inst->username);
    append("password", inst->password);
    if (inst->connect_timeout_ms > 0) {
        char buf[16];
        // Integer seconds — libpq rejects fractional connect_timeout with a
        // hard parse error (see the sync path above for the rounding rule).
        const int secs = (inst->connect_timeout_ms + 999) / 1000;
        std::snprintf(buf, sizeof(buf), "%d", secs);
        append("connect_timeout", buf);
    }
    return s;
}

// Open a fresh PGconn* from the instance's conninfo. Returns nullptr on
// failure (and fills err).
PGconn* open_pg_conn(const pgsql_instance* inst, std::string* err) {
    std::string conninfo = build_conninfo(inst);
    PGconn* pg = PQconnectdb(conninfo.c_str());
    if (!pg) {
        if (err) *err = "PQconnectdb returned NULL (out of memory)";
        return nullptr;
    }
    if (PQstatus(pg) != CONNECTION_OK) {
        if (err) *err = PQerrorMessage(pg);
        PQfinish(pg);
        return nullptr;
    }
    return pg;
}
}  // namespace

// Acquire a connection from the pool. Grows the pool up to `pool_size` when
// the free-list is empty; waits on the condition variable otherwise.
// Returns an empty guard and fills *err on failure (connect error or
// acquire timeout).
//
// Acquire timeout reuses connect_timeout_ms — waiting forever on a
// exhausted pool would deadlock request fibers, and connect_timeout_ms is
// already the user-tuned "give up" knob.
pool_guard acquire_conn(pgsql_instance* inst, std::string* err) {
    if (!inst) {
        if (err) *err = "postgresql: null instance";
        return pool_guard{};
    }

    std::unique_lock<std::mutex> lk(inst->pool_mu);
    if (inst->free_list.empty() && inst->current_size < inst->pool_size) {
        // Grow the pool. Drop the lock around PQconnectdb (network I/O).
        int target = inst->current_size + 1;
        inst->current_size = target;  // reserve the slot under lock
        lk.unlock();
        std::string open_err;
        PGconn* pg = open_pg_conn(inst, &open_err);
        if (!pg) {
            lk.lock();
            // Roll back the slot reservation; a waiter may now grow.
            if (inst->current_size > 0) --inst->current_size;
            inst->pool_cv.notify_one();
            if (err) *err = open_err;
            return pool_guard{};
        }
        return pool_guard{inst, pg};
    }

    if (!inst->free_list.empty()) {
        PGconn* pg = inst->free_list.front();
        inst->free_list.pop();
        lk.unlock();
        // Lazy health check: libpq doesn't auto-detect dropped TCP, so
        // test the connection before handing it out. If it's dead, free
        // it and recurse to get another (or grow).
        if (PQstatus(pg) != CONNECTION_OK) {
            PQfinish(pg);
            std::lock_guard lk2(inst->pool_mu);
            if (inst->current_size > 0) --inst->current_size;
            inst->pool_cv.notify_one();
            return acquire_conn(inst, err);
        }
        return pool_guard{inst, pg};
    }

    // Pool exhausted and at capacity: wait for a release.
    int timeout_ms =
        inst->connect_timeout_ms > 0 ? inst->connect_timeout_ms : 5000;
    if (inst->pool_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                               [inst] { return !inst->free_list.empty(); })) {
        PGconn* pg = inst->free_list.front();
        inst->free_list.pop();
        lk.unlock();
        if (PQstatus(pg) != CONNECTION_OK) {
            PQfinish(pg);
            std::lock_guard lk2(inst->pool_mu);
            if (inst->current_size > 0) --inst->current_size;
            inst->pool_cv.notify_one();
            return acquire_conn(inst, err);
        }
        return pool_guard{inst, pg};
    }

    if (err) *err = "postgresql: connection pool exhausted (acquire timeout)";
    return pool_guard{};
}

// Drain the pool under the lock: PQfinish every conn in the free-list.
// Called from shutdown() before `delete inst`. At this point the instance
// has been unregistered from the global map, so no new Lua proxy can grab
// it; in-flight conns that are checked out will leak (acceptable for
// shutdown — the host has already quiesced the plugin).
void drain_pool(pgsql_instance* inst) {
    std::lock_guard lk(inst->pool_mu);
    while (!inst->free_list.empty()) {
        PGconn* pg = inst->free_list.front();
        inst->free_list.pop();
        if (pg) PQfinish(pg);
        if (inst->current_size > 0) --inst->current_size;
    }
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
int caller_timeout_ms(const pgsql_instance* inst) {
    return inst->call_timeout_ms > 0 ? inst->call_timeout_ms
                                     : inst->query_timeout_ms + 500;
}

namespace {
// JSON params bound as text (the worker twin of collect_lua_params — libpq
// parses them per-column via PQexecParams). storage must outlive the call.
struct json_params {
    std::vector<std::string> storage;
    std::vector<const char*> ptrs;
};

json_params make_json_params(const nlohmann::json& params) {
    json_params out;
    if (!params.is_array()) return out;
    out.storage.reserve(params.size());
    out.ptrs.reserve(params.size());
    for (const auto& v : params) {
        if (v.is_null()) {
            out.storage.emplace_back();
            out.ptrs.push_back(nullptr);
        } else if (v.is_boolean()) {
            out.storage.push_back(v.get<bool>() ? "t" : "f");
            out.ptrs.push_back(out.storage.back().c_str());
        } else if (v.is_number_integer()) {
            out.storage.push_back(std::to_string(v.get<int64_t>()));
            out.ptrs.push_back(out.storage.back().c_str());
        } else if (v.is_number_float()) {
            out.storage.push_back(std::to_string(v.get<double>()));
            out.ptrs.push_back(out.storage.back().c_str());
        } else {
            out.storage.push_back(v.get<std::string>());
            out.ptrs.push_back(out.storage.back().c_str());
        }
    }
    return out;
}

// JSON twin of pg_cell_to_lua: rely on the field's OID only to distinguish
// "definitely integer" (INT2/INT4/INT8/OID) from "definitely float"
// (FLOAT4/FLOAT8/NUMERIC) and boolean (BOOL) from text. NULL stays null.
nlohmann::json pg_cell_to_json(PGresult* res, int row, int col) {
    if (PQgetisnull(res, row, col)) return nullptr;
    const char* v = PQgetvalue(res, row, col);
    if (!v) return nullptr;
    Oid t = PQftype(res, col);
    std::string s = v;
    // Numeric types: INT2=21, INT4=23, INT8=20, OID=26.
    if (t == 21 || t == 23 || t == 20 || t == 26) {
        try {
            return nlohmann::json(std::strtoll(v, nullptr, 10));
        } catch (...) {
            // fall through to text
        }
    }
    if (t == 16) {
        // boolean — libpq renders 't'/'f'.
        return nlohmann::json(s == "t");
    }
    if (t == 700 || t == 701 || t == 1700) {
        try {
            return nlohmann::json(std::stod(s));
        } catch (...) {
            // fall through to text
        }
    }
    return nlohmann::json(std::move(s));
}

nlohmann::json pg_row_to_json(PGresult* res, int r) {
    nlohmann::json row = nlohmann::json::object();
    int n = PQnfields(res);
    for (int c = 0; c < n; ++c) {
        const char* name = PQfname(res, c);
        row[name ? name : "_"] = pg_cell_to_json(res, r, c);
    }
    return row;
}

// Worker-thread statement runner: the JSON twin of run_statement — same three
// modes, same result shapes (query -> array of row objects, query_one -> row
// object or null, execute -> {affected}).
bool run_statement_json(PGconn* conn, const std::string& sql,
                        const nlohmann::json& params, const std::string& mode,
                        nlohmann::json* out, std::string* err_code,
                        std::string* err_msg) {
    json_params jp = make_json_params(params);
    std::string rewritten = rewrite_placeholders(sql.c_str());
    PGresult* res =
        PQexecParams(conn, rewritten.c_str(), static_cast<int>(jp.ptrs.size()),
                     nullptr, jp.ptrs.data(), nullptr, nullptr, 0);
    if (!res) {
        // Out of memory on libpq side — conn is almost certainly toast.
        *err_code = "connection_lost";
        *err_msg = "postgresql: PQexecParams returned NULL";
        return false;
    }
    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        *err_code = map_sqlstate(sqlstate);
        const char* m = PQresultErrorMessage(res);
        *err_msg = m && m[0] ? m : "postgresql: query failed";
        PQclear(res);
        return false;
    }

    if (mode == "execute") {
        char* affected = PQcmdTuples(res);
        *out = nlohmann::json::object(
            {{"affected",
              affected && affected[0]
                  ? static_cast<int64_t>(std::strtoll(affected, nullptr, 10))
                  : 0}});
    } else if (mode == "query_one") {
        // if/else, not a ternary: mixed json/nullptr arms fail to compile.
        if (PQntuples(res) > 0) {
            *out = pg_row_to_json(res, 0);
        } else {
            *out = nullptr;  // no rows
        }
    } else {  // "query"
        nlohmann::json rows = nlohmann::json::array();
        int n = PQntuples(res);
        for (int r = 0; r < n; ++r) {
            rows.push_back(pg_row_to_json(res, r));
        }
        *out = std::move(rows);
    }
    PQclear(res);
    return true;
}

// Worker twin of the sync transaction's BEGIN/COMMIT/ROLLBACK step.
bool run_tx_sql(PGconn* conn, const char* sql, std::string* code,
                std::string* msg) {
    PGresult* res = PQexec(conn, sql);
    if (!res) {
        *code = "connection_lost";
        *msg = std::string(sql) + ": PQexec returned NULL";
        return false;
    }
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        *code = map_sqlstate(PQresultErrorField(res, PG_DIAG_SQLSTATE));
        *msg = std::string(sql) + ": " + PQresultErrorMessage(res);
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

// Deliver one completion. Returns true when the suspended caller claimed it.
bool resume_caller(pgsql_instance* inst, uint64_t session, bool ok,
                   const nlohmann::json& payload) {
    if (inst->host_api == nullptr ||
        inst->host_api->lua_resume_session == nullptr) {
        return false;
    }
    const std::string str = payload.dump();
    return inst->host_api->lua_resume_session(inst->ctx, session, ok ? 1 : 0,
                                              str.c_str()) == 0;
}

void warn_rejected(pgsql_instance* inst, uint64_t session, const char* what) {
    if (inst->host_api == nullptr || inst->host_api->log == nullptr) return;
    std::string msg = std::string("async ") + what + " completion rejected " +
                      "for session " + std::to_string(session) +
                      " (caller timeout or service gone)";
    inst->host_api->log(SHIELD_LOG_WARN, "database.postgresql",
                        inst->instance_id.c_str(), msg.c_str());
}

// Poison the connection a just-failed statement ran on (same codes the sync
// path marks bad).
void mark_bad_on_conn_error(const std::string& code, pool_guard* guard) {
    if (code == "connection_lost" || code == "connection_timeout") {
        guard->bad = true;
    }
}

// Execute one task off the actor thread. THE M3 POINT — the pool acquire for
// plain statements and BEGIN runs here, so a pool exhausted by concurrent
// holders suspends the calling coroutine instead of blocking the actor. M4
// adds the token-bound tx forms running on the held connection.
void run_task(pgsql_instance* inst, pg_task task) {
    bool ok = false;
    nlohmann::json result;  // success payload value
    nlohmann::json err;     // {code, message} on failure
    std::unique_ptr<pool_guard> guard;
    uint64_t new_token = 0;  // set by a successful tx_begin

    if (task.kind == pg_task_kind::stmt) {
        std::string acquire_err;
        guard = std::make_unique<pool_guard>(acquire_conn(inst, &acquire_err));
        if (guard->get()) {
            std::string err_code;
            std::string err_msg;
            ok = run_statement_json(guard->get(), task.sql, task.params,
                                    task.method, &result, &err_code, &err_msg);
            if (!ok) {
                mark_bad_on_conn_error(err_code, guard.get());
                err = {{"code", err_code}, {"message", err_msg}};
            }
        } else {
            err = {{"code", "connection_failed"},
                   {"message", acquire_err.empty()
                                   ? std::string("pool acquire failed")
                                   : acquire_err}};
        }
    } else if (task.kind == pg_task_kind::tx_begin) {
        std::string acquire_err;
        guard = std::make_unique<pool_guard>(acquire_conn(inst, &acquire_err));
        if (!guard->get()) {
            err = {{"code", "connection_failed"},
                   {"message", acquire_err.empty()
                                   ? std::string("pool acquire failed")
                                   : acquire_err}};
        } else {
            std::string code;
            std::string msg;
            if (run_tx_sql(guard->get(), "BEGIN", &code, &msg)) {
                new_token = inst->next_tx_token.fetch_add(1);
                {
                    std::lock_guard lk(inst->worker_mu);
                    inst->held_tx[new_token] = std::move(guard);
                }
                ok = true;
                result = new_token;  // the shim carries this on every tx task
            } else {
                guard->bad = true;
                err = {{"code", code}, {"message", msg}};
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
            if (task.kind != pg_task_kind::tx_stmt) {
                inst->held_tx.erase(task.tx_token);
            }
        }
        if (!guard->get()) {
            err = {{"code", "transaction_closed"},
                   {"message", "transaction is no longer open"}};
        } else if (task.kind == pg_task_kind::tx_stmt) {
            std::string err_code;
            std::string err_msg;
            ok = run_statement_json(guard->get(), task.sql, task.params,
                                    task.method, &result, &err_code, &err_msg);
            if (!ok) {
                mark_bad_on_conn_error(err_code, guard.get());
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
            std::string code;
            std::string msg;
            const char* sql =
                task.kind == pg_task_kind::tx_commit ? "COMMIT" : "ROLLBACK";
            if (run_tx_sql(guard->get(), sql, &code, &msg)) {
                ok = true;
                result = true;
            } else {
                guard->bad = true;
                err = {{"code", code}, {"message", msg}};
            }
            // guard's destructor releases: recycled when healthy, PQfinish'ed
            // when bad.
        }
    }

    // Payload = the VALUES array only — resume_suspended_caller pushes the
    // ok boolean itself, so the shim sees (ok, value...) from [value] and
    // (false, err_table) from [{code, message}].
    const nlohmann::json payload =
        ok ? nlohmann::json::array({result}) : nlohmann::json::array({err});
    if (!resume_caller(inst, task.session, ok, payload)) {
        if (task.kind == pg_task_kind::stmt) {
            // Discarded statement result — poison so it is never reused.
            if (guard && guard->get()) guard->bad = true;
        } else if (task.kind == pg_task_kind::tx_begin && new_token != 0) {
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
            if (g && g->get()) {
                std::string code;
                std::string msg;
                if (!run_tx_sql(g->get(), "ROLLBACK", &code, &msg)) {
                    g->bad = true;
                }
            }
        }
        // tx_stmt / tx_commit / tx_rollback rejections need no extra work:
        // the shim's rollback (queued behind tx_stmt on the tx lane) or the
        // shutdown drain owns the connection.
        warn_rejected(inst, task.session,
                      task.kind == pg_task_kind::stmt ? "statement" : "tx");
    }
    inst->pending_async.fetch_sub(1, std::memory_order_relaxed);
}

// Worker body: drain one queue until stopped. tx_lane == true serves the
// serialized transaction queue; general workers serve pool statements and
// BEGIN (BEGIN stays off the tx lane so a pool-exhausted acquire can never
// block another transaction's commit behind it).
void task_loop(pgsql_instance* inst, bool tx_lane) {
    std::unique_lock lk(inst->worker_mu);
    while (true) {
        inst->worker_cv.wait(lk, [&] {
            return inst->worker_stop || (tx_lane ? !inst->tx_tasks.empty()
                                                 : !inst->worker_tasks.empty());
        });
        std::deque<pg_task>& q = tx_lane ? inst->tx_tasks : inst->worker_tasks;
        if (q.empty()) {
            if (inst->worker_stop) return;
            continue;
        }
        pg_task task = std::move(q.front());
        q.pop_front();
        lk.unlock();
        run_task(inst, std::move(task));
        lk.lock();
    }
}

// Worker pool size: never more than the connection pool (extra workers would
// only queue on acquire), hard-capped so thread counts stay sane.
int worker_count(const pgsql_instance* inst) {
    int n = inst->pool_size > 0 ? inst->pool_size : 1;
    return std::min(n, 8);
}
}  // namespace

// Submit one task: lazily start the worker pool (and the tx lane for
// token-bound tasks), enqueue, wake. Runs on the actor thread right after the
// caller suspended — the task may therefore complete before the shim reaches
// coroutine.yield(); the host's yield-window guard requeues that completion
// over the actor mailbox.
void submit_async(pgsql_instance* inst, pg_task task) {
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
        const bool lane = task.kind != pg_task_kind::stmt &&
                          task.kind != pg_task_kind::tx_begin;
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
void drain_held_tx(pgsql_instance* inst) {
    std::vector<std::unique_ptr<pool_guard>> guards;
    {
        std::lock_guard lk(inst->worker_mu);
        guards.reserve(inst->held_tx.size());
        for (auto& [tok, g] : inst->held_tx) guards.push_back(std::move(g));
        inst->held_tx.clear();
    }
    for (auto& g : guards) {
        if (g && g->get()) {
            std::string code;
            std::string msg;
            if (!run_tx_sql(g->get(), "ROLLBACK", &code, &msg)) g->bad = true;
        }
        g.reset();  // release: recycled when healthy, PQfinish'ed when bad
    }
}

namespace {
// shield.pool.stats.v1 — real gauges from the per-instance pool. The driver
// tracks capacity/usage but no cumulative counters, so those stay -1
// (unknown), matching the ABI's sentinel semantics.
int pg_pool_get_stats(struct shield_plugin_instance_v1* self,
                      struct shield_pool_stats* out) {
    auto* inst = reinterpret_cast<pgsql_instance*>(self);
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

const shield_pool_stats_v1& pg_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &pg_pool_get_stats,
    };
    return v;
}
