// [SHIELD_PLUGIN] database.sqlite — instance registry and the async
// engine: the process-wide instance map, the per-instance worker
// thread with its JSON statement runner and transaction holds, and
// the pool-stats surface (async gauges only; sqlite has no pool).
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

// Process-wide registry: instance_id -> sqlite_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Map is read on every proxy creation, so it must be
// thread-safe.
// Intentionally leaked (never destructed): PluginHost's own static destructor
// can run instance shutdown() at process exit, and by then function-local
// statics initialized after the host — these — would already be gone.
// unregister_instance() walking a destroyed map is a use-after-free (caught
// by valgrind in the async-transaction tests).

namespace {
std::mutex& instances_mu() {
    static std::mutex& m = *new std::mutex;
    return m;
}
std::map<std::string, sqlite_instance*>& instances_map() {
    static std::map<std::string, sqlite_instance*>& m =
        *new std::map<std::string, sqlite_instance*>;
    return m;
}
}  // namespace

void register_instance(sqlite_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
sqlite_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract the two known keys and
// fall back to defaults for anything missing.
void parse_instance_config(sqlite_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("database") && j["database"].is_string()) {
            inst->database_path = j["database"].get<std::string>();
        }
        if (j.contains("query_timeout_ms") &&
            j["query_timeout_ms"].is_number_integer()) {
            inst->query_timeout_ms = j["query_timeout_ms"].get<int>();
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
                inst->host_api->log(SHIELD_LOG_WARN, "database.sqlite",
                                    inst->instance_id.c_str(), buf);
            }
        }
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

// ---------------------------------------------------------------------------
// Lua helpers.
//
// SQLite is embedded — there is no connection pool. Each proxy call opens a
// fresh sqlite3*, runs the statement, and closes. open_connection() returns
// nullptr on failure (and fills an error message).
// ---------------------------------------------------------------------------

sqlite3* open_connection(const sqlite_instance* inst, std::string* err_msg) {
    sqlite3* db = nullptr;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI |
                SQLITE_OPEN_NOMUTEX;
    int rc = sqlite3_open_v2(inst->database_path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            const char* m = db ? sqlite3_errmsg(db) : "sqlite open failed";
            *err_msg = m;
        }
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, inst->query_timeout_ms);
    return db;
}

// ---------------------------------------------------------------------------
// Async path (docs/db-async-design.md M2). The proxy methods hand SQL to a
// per-instance worker thread and suspend the calling coroutine; completion
// travels back through host_api->lua_resume_session. The worker never touches
// a lua_State — it speaks JSON in both directions, and the host's resume
// converts the payload back into Lua values.
// ---------------------------------------------------------------------------

// Caller-side suspend budget (design doc: query_timeout_ms + 500ms slack
// unless explicitly overridden).
int caller_timeout_ms(const sqlite_instance* inst) {
    return inst->call_timeout_ms > 0 ? inst->call_timeout_ms
                                     : inst->query_timeout_ms + 500;
}

namespace {
// JSON twin of row_to_lua: column-name keyed object. NULL columns become
// JSON null (the host pushes Lua nil for them, matching the sync shape).
nlohmann::json row_to_json(sqlite3_stmt* stmt) {
    nlohmann::json row = nlohmann::json::object();
    int n = sqlite3_column_count(stmt);
    for (int c = 0; c < n; ++c) {
        const char* name = sqlite3_column_name(stmt, c);
        switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_INTEGER:
                row[name] = sqlite3_column_int64(stmt, c);
                break;
            case SQLITE_FLOAT:
                row[name] = sqlite3_column_double(stmt, c);
                break;
            case SQLITE_TEXT: {
                const unsigned char* t = sqlite3_column_text(stmt, c);
                int sz = sqlite3_column_bytes(stmt, c);
                // if/else, not a ternary: mixed std::string/json arms fail
                // to compile.
                if (t) {
                    row[name] = std::string(reinterpret_cast<const char*>(t),
                                            static_cast<size_t>(sz));
                } else {
                    row[name] = nullptr;
                }
                break;
            }
            case SQLITE_BLOB: {
                const char* b =
                    static_cast<const char*>(sqlite3_column_blob(stmt, c));
                int sz = sqlite3_column_bytes(stmt, c);
                if (b) {
                    row[name] = std::string(b, static_cast<size_t>(sz));
                } else {
                    row[name] = nullptr;
                }
                break;
            }
            default:
                row[name] = nullptr;  // SQLITE_NULL
                break;
        }
    }
    return row;
}

// Worker-thread statement runner: the JSON twin of run_statement — same
// three modes, same result shapes (query -> array of row objects,
// query_one -> row object or null, execute -> {affected, last_insert_id}).
bool run_statement_json(sqlite3* db, const std::string& sql,
                        const nlohmann::json& params, const std::string& mode,
                        nlohmann::json* out, std::string* err_code,
                        std::string* err_msg) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        *err_code = map_sqlite_error(rc);
        *err_msg = sqlite3_errmsg(db);
        return false;
    }
    // Positional bind, 1..N of a JSON array (mirrors bind_lua_param's type
    // mapping).
    if (params.is_array()) {
        int idx = 1;
        for (const auto& v : params) {
            int brc = SQLITE_OK;
            if (v.is_boolean()) {
                brc = sqlite3_bind_int(stmt, idx, v.get<bool>() ? 1 : 0);
            } else if (v.is_number_integer()) {
                brc = sqlite3_bind_int64(stmt, idx, v.get<int64_t>());
            } else if (v.is_number_float()) {
                brc = sqlite3_bind_double(stmt, idx, v.get<double>());
            } else if (v.is_string()) {
                const auto& s = v.get_ref<const std::string&>();
                brc = sqlite3_bind_text(stmt, idx, s.data(),
                                        static_cast<int>(s.size()),
                                        SQLITE_TRANSIENT);
            } else {
                brc = sqlite3_bind_null(stmt, idx);
            }
            if (brc != SQLITE_OK) {
                sqlite3_finalize(stmt);
                *err_code = map_sqlite_error(brc);
                *err_msg = sqlite3_errmsg(db);
                return false;
            }
            ++idx;
        }
    }

    bool ok = false;
    if (mode == "execute") {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
            *out = nlohmann::json::object(
                {{"affected", sqlite3_changes(db)},
                 {"last_insert_id", sqlite3_last_insert_rowid(db)}});
            ok = true;
        } else {
            *err_code = map_sqlite_error(rc);
            *err_msg = sqlite3_errmsg(db);
        }
    } else if (mode == "query_one") {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            *out = row_to_json(stmt);
            ok = true;
        } else if (rc == SQLITE_DONE) {
            *out = nullptr;  // no rows
            ok = true;
        } else {
            *err_code = map_sqlite_error(rc);
            *err_msg = sqlite3_errmsg(db);
        }
    } else {  // "query"
        nlohmann::json rows = nlohmann::json::array();
        while (true) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                rows.push_back(row_to_json(stmt));
            } else if (rc == SQLITE_DONE) {
                *out = std::move(rows);
                ok = true;
                break;
            } else {
                *err_code = map_sqlite_error(rc);
                *err_msg = sqlite3_errmsg(db);
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

// Worker body: drain tasks until stopped. Each task runs the SQL on a fresh
// connection (same semantics as the sync path) and completes the suspended
// caller through the host's resume primitive — safe from any thread, the
// host routes it over the caller actor's mailbox.
// One BEGIN/COMMIT/ROLLBACK step on a held handle (sqlite3_exec wrapper).
bool run_tx_sql(sqlite3* db, const char* sql, std::string* msg) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK) return true;
    *msg = err ? err : std::string(sql) + " failed";
    sqlite3_free(err);
    return false;
}

// Deliver one completion. Returns true when the suspended caller claimed it.
bool resume_caller(sqlite_instance* inst, uint64_t session, bool ok,
                   const nlohmann::json& payload) {
    if (inst->host_api == nullptr ||
        inst->host_api->lua_resume_session == nullptr) {
        return false;
    }
    const std::string str = payload.dump();
    return inst->host_api->lua_resume_session(inst->ctx, session, ok ? 1 : 0,
                                              str.c_str()) == 0;
}
}  // namespace

namespace {
// Execute one task off the actor thread. sqlite opens a connection per call;
// the transaction forms instead hold one handle for the tx's lifetime.
void run_task(sqlite_instance* inst, sqlite_task task) {
    bool ok = false;
    nlohmann::json result;  // success payload value
    nlohmann::json err;     // {code, message} on failure
    sqlite3* db = nullptr;  // owned here unless handed to held_tx
    uint64_t new_token = 0;

    if (task.kind == task_kind::stmt) {
        std::string open_err;
        db = open_connection(inst, &open_err);
        if (db) {
            std::string err_code;
            std::string err_msg;
            ok = run_statement_json(db, task.sql, task.params, task.method,
                                    &result, &err_code, &err_msg);
            if (!ok) err = {{"code", err_code}, {"message", err_msg}};
            // sqlite keeps no pool: per-call open/close means the
            // design-doc's "poison the connection" rule degenerates to
            // plain close — a timed-out worker's connection is never
            // reused, which is the invariant the rule protects.
            sqlite3_close(db);
            db = nullptr;
        } else {
            err = {{"code", "connection_failed"}, {"message", open_err}};
        }
    } else if (task.kind == task_kind::tx_begin) {
        std::string open_err;
        db = open_connection(inst, &open_err);
        if (!db) {
            err = {{"code", "connection_failed"}, {"message", open_err}};
        } else {
            std::string msg;
            if (run_tx_sql(db, "BEGIN", &msg)) {
                new_token = inst->next_tx_token.fetch_add(1);
                {
                    std::lock_guard lk(inst->worker_mu);
                    inst->held_tx[new_token] = db;
                }
                db = nullptr;  // owned by the held map now
                ok = true;
                result = new_token;  // the shim carries this on every tx task
            } else {
                sqlite3_close(db);
                db = nullptr;
                err = {{"code", "db_query_failed"}, {"message", msg}};
            }
        }
    } else {
        // Token-bound: statement / COMMIT / ROLLBACK on a held handle.
        {
            std::lock_guard lk(inst->worker_mu);
            auto it = inst->held_tx.find(task.tx_token);
            if (it != inst->held_tx.end()) {
                db = it->second;
                // End-of-tx tasks always take the token out: whatever
                // happens, the handle is closed below.
                if (task.kind != task_kind::tx_stmt) {
                    inst->held_tx.erase(it);
                }
            }
        }
        if (db == nullptr) {
            err = {{"code", "transaction_closed"},
                   {"message", "transaction is no longer open"}};
        } else if (task.kind == task_kind::tx_stmt) {
            std::string err_code;
            std::string err_msg;
            ok = run_statement_json(db, task.sql, task.params, task.method,
                                    &result, &err_code, &err_msg);
            if (!ok) err = {{"code", err_code}, {"message", err_msg}};
            // The handle stays in held_tx: the caller's next statement — or
            // the rollback a caller timeout triggers — must find it.
        } else {
            std::string msg;
            const char* sql =
                task.kind == task_kind::tx_commit ? "COMMIT" : "ROLLBACK";
            if (run_tx_sql(db, sql, &msg)) {
                ok = true;
                result = true;
            } else {
                err = {{"code", "db_query_failed"}, {"message", msg}};
            }
            sqlite3_close(db);  // no pool: the tx handle dies with the tx
            db = nullptr;
        }
    }

    // Payload = the VALUES array only — resume_suspended_caller pushes the
    // ok boolean itself, so the shim sees (ok, value...) from [value] and
    // (false, err_table) from [{code, message}].
    const nlohmann::json payload =
        ok ? nlohmann::json::array({result}) : nlohmann::json::array({err});
    if (!resume_caller(inst, task.session, ok, payload)) {
        if (task.kind == task_kind::tx_begin && new_token != 0) {
            // Nobody can commit or roll back a tx whose caller is gone: take
            // it back and end it. ROLLBACK is deterministic here, so the
            // handle just closes.
            std::lock_guard lk(inst->worker_mu);
            auto it = inst->held_tx.find(new_token);
            if (it != inst->held_tx.end()) {
                std::string msg;
                run_tx_sql(it->second, "ROLLBACK", &msg);
                sqlite3_close(it->second);
                inst->held_tx.erase(it);
            }
        }
        // tx_stmt / tx_commit / tx_rollback rejections need no extra work:
        // the shim's rollback (queued behind tx_stmt on the single worker)
        // or the shutdown drain owns the handle.
        if (inst->host_api != nullptr && inst->host_api->log != nullptr) {
            const char* what =
                task.kind == task_kind::stmt ? "statement" : "tx";
            std::string msg = std::string("async ") + what +
                              " completion rejected for session " +
                              std::to_string(task.session) +
                              " (caller timeout or service gone)";
            inst->host_api->log(SHIELD_LOG_WARN, "database.sqlite",
                                inst->instance_id.c_str(), msg.c_str());
        }
    }
    inst->pending_async.fetch_sub(1, std::memory_order_relaxed);
}

void worker_loop(sqlite_instance* inst) {
    std::unique_lock lk(inst->worker_mu);
    while (true) {
        inst->worker_cv.wait(lk, [&] {
            return inst->worker_stop || !inst->worker_tasks.empty();
        });
        if (inst->worker_tasks.empty()) {
            if (inst->worker_stop) return;
            continue;
        }
        sqlite_task task = std::move(inst->worker_tasks.front());
        inst->worker_tasks.pop_front();
        lk.unlock();
        run_task(inst, std::move(task));
        lk.lock();
    }
}
}  // namespace

// Roll back and close every still-held transaction handle (shutdown path:
// after the worker is joined nothing can end them on their own).
void drain_held_tx(sqlite_instance* inst) {
    std::lock_guard lk(inst->worker_mu);
    for (auto& [tok, db] : inst->held_tx) {
        std::string msg;
        run_tx_sql(db, "ROLLBACK", &msg);
        sqlite3_close(db);
    }
    inst->held_tx.clear();
}

// Submit one task: lazily start the worker, enqueue, wake it. Runs on the
// actor thread right after the caller suspended — the task may therefore
// complete before the shim reaches coroutine.yield(); the host's
// yield-window guard requeues that completion over the actor mailbox.
void submit_async(sqlite_instance* inst, sqlite_task task) {
    {
        std::lock_guard lk(inst->worker_mu);
        if (!inst->worker_started) {
            inst->worker_started = true;
            inst->worker_thread = std::thread(worker_loop, inst);
        }
        inst->worker_tasks.push_back(std::move(task));
        inst->pending_async.fetch_add(1, std::memory_order_relaxed);
    }
    inst->worker_cv.notify_one();
}

namespace {
// shield.pool.stats.v1 — sqlite keeps no connection pool (one connection per
// call), so the pool gauges report the ABI's -1 "not applicable" sentinel;
// the two async gauges are real. holding counts transactions whose handle is
// currently checked out to a suspended body.
int sqlite_pool_get_stats(struct shield_plugin_instance_v1* self,
                          struct shield_pool_stats* out) {
    auto* inst = reinterpret_cast<sqlite_instance*>(self);
    if (!inst || !out) return -1;
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
    {
        std::lock_guard lk(inst->worker_mu);
        out->holding = static_cast<int>(inst->held_tx.size());
    }
    out->pending_async = inst->pending_async.load();
    return 0;
}
}  // namespace

const shield_pool_stats_v1& sqlite_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &sqlite_pool_get_stats,
    };
    return v;
}
