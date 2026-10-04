// [SHIELD_PLUGIN] database.sqlite — SQLite provider for shield.database.v1.
//
// Implements the new v1 ABI (shield_plugin_get_v1) on top of the sqlite3 C
// API. SQLite is a single-file embedded database: host/port/user/password are
// ignored, and `database` (passed via shield_db_connect_args at connect time)
// is interpreted as a filesystem path (":memory:" for in-memory).
//
// The SQL surface (connect/disconnect/ping/query/execute/begin/commit/
// rollback/free_result) is inherited verbatim from the legacy shield_db_plugin
// ABI; the per-instance pool and Lua callable namespace are owned by this
// plugin (no host-side facade remains).

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
#include "shield_lua_plugin_binding.hpp"

// shield_db_conn is forward-declared as opaque in database.h; the concrete
// layout is defined here (global scope, matching the header's declaration)
// so db_vtable() below can allocate/inspect it.
struct shield_db_conn {
    sqlite3* db;
};

namespace {

// Copy a sqlite3 column text (may be NULL/static) into a malloc'd buffer the
// host can later free(). Returns NULL when the SQL value was NULL.
char* dup_text_nullsafe(const unsigned char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(reinterpret_cast<const char*>(s));
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

// Map a sqlite3 result code to a stable shield error code.
const char* map_sqlite_error(int rc) {
    switch (rc) {
        case SQLITE_BUSY:
            return "connection_timeout";
        case SQLITE_CONSTRAINT:
            return "constraint_violation";
        case SQLITE_MISMATCH:
            return "syntax_error";
        case SQLITE_READONLY:
        case SQLITE_PERM:
            return "auth_failed";
        case SQLITE_NOMEM:
            return "pool_exhausted";
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return "db_query_failed";
        default:
            return "db_query_failed";
    }
}

// Release heap memory referenced by a result struct. Does not free `result`
// itself (host owns the struct).
void clear_result(shield_db_result* r) {
    if (!r) return;
    if (r->error_msg) {
        std::free(const_cast<char*>(r->error_msg));
        r->error_msg = nullptr;
    }
    if (r->error_code) {
        std::free(const_cast<char*>(r->error_code));
        r->error_code = nullptr;
    }
    if (r->cells) {
        int n = r->row_count * r->col_count;
        for (int i = 0; i < n; ++i) {
            if (r->cells[i]) std::free(const_cast<char*>(r->cells[i]));
        }
        std::free(const_cast<char**>(r->cells));
        r->cells = nullptr;
    }
    r->row_count = 0;
    r->col_count = 0;
}

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

// Fill `out` with an error from `db`. Always returns 0 (soft failure).
int fill_sqlite_error(sqlite3* db, int rc, shield_db_result* out) {
    out->success = 0;
    const char* msg = sqlite3_errmsg(db);
    out->error_msg = dup_string(msg ? msg : "unknown sqlite error");
    out->error_code = dup_string(map_sqlite_error(rc));
    return 0;
}

// Execute a prepared statement with text-bound params. On success fills `out`
// with rows (SELECT) or affected count (DML). Returns 0 on success or soft
// SQL failure, non-zero on hard error.
int run_prepared(sqlite3* db, const char* sql, const char* const* params,
                 int n_params, shield_db_result* out) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return fill_sqlite_error(db, rc, out);
    }
    for (int i = 0; i < n_params; ++i) {
        const char* p = params ? params[i] : nullptr;
        rc = p ? sqlite3_bind_text(stmt, i + 1, p, -1, SQLITE_TRANSIENT)
               : sqlite3_bind_null(stmt, i + 1);
        if (rc != SQLITE_OK) {
            int local = rc;
            sqlite3_finalize(stmt);
            return fill_sqlite_error(db, local, out);
        }
    }
    int col_count = sqlite3_column_count(stmt);
    std::vector<const char*> cells;
    cells.reserve(static_cast<size_t>(col_count) * 4);
    int row_count = 0;
    int64_t affected = 0;
    while (true) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            for (int c = 0; c < col_count; ++c) {
                cells.push_back(
                    dup_text_nullsafe(sqlite3_column_text(stmt, c)));
            }
            ++row_count;
        } else if (rc == SQLITE_DONE) {
            affected = static_cast<int64_t>(sqlite3_changes(db));
            break;
        } else {
            sqlite3_finalize(stmt);
            for (const char* cell : cells) std::free(const_cast<char*>(cell));
            return fill_sqlite_error(db, rc, out);
        }
    }
    sqlite3_finalize(stmt);
    out->success = 1;
    out->error_msg = nullptr;
    out->error_code = nullptr;
    out->affected_rows = affected;
    out->last_insert_id = static_cast<int64_t>(sqlite3_last_insert_rowid(db));
    out->row_count = row_count;
    out->col_count = col_count;
    out->cells = row_count > 0 ? static_cast<const char**>(
                                     std::malloc(sizeof(char*) * cells.size()))
                               : nullptr;
    if (out->cells) {
        std::memcpy(const_cast<char**>(out->cells), cells.data(),
                    sizeof(char*) * cells.size());
    }
    return 0;
}

// The shield.database.v1 vtable. Stateless — all per-connection state lives
// in shield_db_conn (handed out by connect). All instances share this table.
const shield_database_v1& db_vtable() {
    static const shield_database_v1 v = {
        sizeof(shield_database_v1),
        SHIELD_DATABASE_INTERFACE,
        "sqlite",
        "1.0.0",
        // connect
        [](const shield_db_connect_args* args, char* err_buf,
           int err_buf_size) -> shield_db_conn* {
            if (!args || !args->database) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size,
                                  "sqlite connect: missing database path");
                return nullptr;
            }
            sqlite3* db = nullptr;
            int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                        SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
            int rc = sqlite3_open_v2(args->database, &db, flags, nullptr);
            if (rc != SQLITE_OK) {
                if (err_buf && err_buf_size > 0) {
                    const char* msg =
                        db ? sqlite3_errmsg(db) : "sqlite open failed";
                    std::snprintf(err_buf, err_buf_size, "%s", msg);
                }
                if (db) sqlite3_close(db);
                return nullptr;
            }
            int timeout_ms =
                args->query_timeout_ms ? args->query_timeout_ms : 5000;
            sqlite3_busy_timeout(db, timeout_ms);
            return new shield_db_conn{db};
        },
        // disconnect
        [](shield_db_conn* c) {
            if (!c) return;
            if (c->db) sqlite3_close(c->db);
            delete c;
        },
        // ping
        [](shield_db_conn* c) -> int {
            if (!c || !c->db) return 0;
            return (sqlite3_db_readonly(c->db, nullptr) >= -1) ? 1 : 0;
        },
        // query
        [](shield_db_conn* c, const char* sql, const char* const* params,
           int n_params, shield_db_result* out) -> int {
            if (!c || !c->db || !sql) {
                out->success = 0;
                out->error_msg = dup_string("sqlite: invalid arguments");
                out->error_code = dup_string("db_query_failed");
                return 1;
            }
            return run_prepared(c->db, sql, params, n_params, out);
        },
        // execute (same path as query; sqlite doesn't distinguish)
        [](shield_db_conn* c, const char* sql, const char* const* params,
           int n_params, shield_db_result* out) -> int {
            if (!c || !c->db || !sql) {
                out->success = 0;
                out->error_msg = dup_string("sqlite: invalid arguments");
                out->error_code = dup_string("db_query_failed");
                return 1;
            }
            return run_prepared(c->db, sql, params, n_params, out);
        },
        // begin / commit / rollback
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->db) return 1;
            return run_prepared(c->db, "BEGIN", nullptr, 0, out);
        },
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->db) return 1;
            return run_prepared(c->db, "COMMIT", nullptr, 0, out);
        },
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->db) return 1;
            return run_prepared(c->db, "ROLLBACK", nullptr, 0, out);
        },
        // free_result
        [](shield_db_result* r) { clear_result(r); },
    };
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json) and registers itself in a process-wide map so the Lua callable
// namespace can resolve plugins.bindings logical names to started instances.
// The C++ vtable is still served through get_interface() for any C-ABI
// caller (none in tree today, but kept for forward compatibility).
// ---------------------------------------------------------------------------
namespace {

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

// Process-wide registry: instance_id -> sqlite_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Map is read on every proxy creation, so it must be
// thread-safe.
// Intentionally leaked (never destructed): PluginHost's own static destructor
// can run instance shutdown() at process exit, and by then function-local
// statics initialized after the host — these — would already be gone.
// unregister_instance() walking a destroyed map is a use-after-free (caught
// by valgrind in the async-transaction tests).
std::mutex& instances_mu() {
    static std::mutex& m = *new std::mutex;
    return m;
}
std::map<std::string, sqlite_instance*>& instances_map() {
    static std::map<std::string, sqlite_instance*>& m =
        *new std::map<std::string, sqlite_instance*>;
    return m;
}

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

// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.database.* facade.
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Bind one Lua value onto a prepared statement at `idx` (1-based). Mapping:
//   nil/invalid      -> NULL
//   bool             -> INTEGER (0/1)
//   lua_Integer      -> INTEGER64 (covers Lua 5.3+ native ints)
//   double           -> FLOAT
//   string           -> TEXT
//   anything else    -> NULL (with a soft warning in *err_msg if provided)
int bind_lua_param(sqlite3_stmt* stmt, int idx, const shd::object& v,
                   std::string* err_msg) {
    if (!v.valid() || v == shd::nil) {
        return sqlite3_bind_null(stmt, idx);
    }
    // Boolean first — sol casts bool to int otherwise.
    if (v.is<bool>()) {
        bool b = v.as<bool>();
        return sqlite3_bind_int(stmt, idx, b ? 1 : 0);
    }
    if (v.is<lua_Integer>()) {
        // Covers Lua 5.3+ native 64-bit integers (including values > INT_MAX).
        return sqlite3_bind_int64(stmt, idx, v.as<lua_Integer>());
    }
    if (v.is<double>()) {
        return sqlite3_bind_double(stmt, idx, v.as<double>());
    }
    if (v.is<std::string>()) {
        std::string s = v.as<std::string>();
        return sqlite3_bind_text(stmt, idx, s.c_str(),
                                 static_cast<int>(s.size()), SQLITE_TRANSIENT);
    }
    // Fallback: bind NULL for anything we can't convert (tables, functions,
    // userdata). This avoids silent mis-binding; caller sees a soft warning.
    if (err_msg)
        *err_msg = "unsupported parameter type at index " +
                   std::to_string(idx) + "; bound as NULL";
    return sqlite3_bind_null(stmt, idx);
}

// Convert the current row of `stmt` into a Lua table keyed by column name.
shd::table row_to_lua(shd::state_view lua, sqlite3_stmt* stmt) {
    auto row = lua.create_table();
    int n = sqlite3_column_count(stmt);
    for (int c = 0; c < n; ++c) {
        const char* name = sqlite3_column_name(stmt, c);
        switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_INTEGER:
                row[name] =
                    static_cast<lua_Integer>(sqlite3_column_int64(stmt, c));
                break;
            case SQLITE_FLOAT:
                row[name] = sqlite3_column_double(stmt, c);
                break;
            case SQLITE_TEXT: {
                const unsigned char* t = sqlite3_column_text(stmt, c);
                // Split into branches: a ternary of std::string vs shd::nil
                // is ambiguous under the facade.
                if (t) {
                    row[name] = std::string(reinterpret_cast<const char*>(t));
                } else {
                    row[name] = shd::nil;
                }
                break;
            }
            case SQLITE_BLOB: {
                const void* b = sqlite3_column_blob(stmt, c);
                int sz = sqlite3_column_bytes(stmt, c);
                if (b) {
                    row[name] = std::string(static_cast<const char*>(b),
                                            static_cast<size_t>(sz));
                } else {
                    row[name] = shd::nil;
                }
                break;
            }
            default:
                row[name] = shd::nil;  // SQLITE_NULL
                break;
        }
    }
    return row;
}

// Prepare + bind + step a statement on the given connection. Used by the
// proxy query/query_one/execute helpers. The `mode` argument selects the
// return shape. On error, *ok is set to false and *err_out receives an error
// table.
//
// Returns one of:
//   query        -> sequence table {row1, row2, ...}
//   query_one    -> single row table or nil
//   execute      -> table {affected=N, last_insert_id=M}
shd::object run_statement(
    shd::state_view lua, sqlite3* db, const std::string& sql,
    std::optional<shd::table> params,
    const char* mode,  // "query" | "query_one" | "execute"
    bool* ok, shd::table* err_out) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        *ok = false;
        *err_out =
            make_error_table(lua, map_sqlite_error(rc), sqlite3_errmsg(db));
        return shd::nil;
    }
    // Bind parameters (Lua sequence table 1..N). Collect positional values
    // first to avoid any iterator invalidation from stack work during bind.
    if (params && params->valid()) {
        std::vector<shd::object> positional;
        for (const auto& kv : *params) {
            if (kv.first.get_type() != shd::type::number) continue;
            positional.push_back(kv.second);
        }
        for (size_t i = 0; i < positional.size(); ++i) {
            std::string bind_err;
            int brc = bind_lua_param(stmt, static_cast<int>(i + 1),
                                     positional[i], &bind_err);
            if (brc != SQLITE_OK) {
                sqlite3_finalize(stmt);
                *ok = false;
                *err_out = make_error_table(
                    lua, map_sqlite_error(brc),
                    bind_err.empty() ? sqlite3_errmsg(db) : bind_err);
                return shd::nil;
            }
        }
    }

    shd::object result = shd::nil;
    if (std::strcmp(mode, "execute") == 0) {
        // Step once; we don't collect rows for DML.
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            *ok = false;
            *err_out =
                make_error_table(lua, map_sqlite_error(rc), sqlite3_errmsg(db));
            return shd::nil;
        }
        auto t = lua.create_table();
        t["affected"] = static_cast<lua_Integer>(sqlite3_changes(db));
        t["last_insert_id"] =
            static_cast<lua_Integer>(sqlite3_last_insert_rowid(db));
        result = t;
    } else if (std::strcmp(mode, "query_one") == 0) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            result = row_to_lua(lua, stmt);
        } else if (rc == SQLITE_DONE) {
            result = shd::object(shd::nil);  // no rows
        } else {
            sqlite3_finalize(stmt);
            *ok = false;
            *err_out =
                make_error_table(lua, map_sqlite_error(rc), sqlite3_errmsg(db));
            return shd::nil;
        }
    } else {  // "query"
        auto rows = lua.create_table();
        int row_index = 1;
        while (true) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                rows[row_index++] = row_to_lua(lua, stmt);
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                sqlite3_finalize(stmt);
                *ok = false;
                *err_out = make_error_table(lua, map_sqlite_error(rc),
                                            sqlite3_errmsg(db));
                return shd::nil;
            }
        }
        result = rows;
    }
    sqlite3_finalize(stmt);
    *ok = true;
    return result;
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

// Build a per-call proxy table bound to a specific sqlite3* handle. Used by
// transaction() so the callback's tx:execute/tx:query share one connection.
shd::table make_handle_proxy(shd::state_view lua, sqlite3* db);

// Async shim installed over the instance proxy: query/query_one/execute
// become submit-and-yield wrappers around the __sync_* implementations.
// The shim source is shared with the mysql/postgresql drivers — see
// plugins/_shared/shield_db_async_shim.hpp for the contract.
constexpr const char* kAsyncShim = shield::plugins::kDbAsyncShimLua;

// Build a per-instance proxy table that opens a fresh connection per call.
shd::table make_instance_proxy(shd::state_view lua, sqlite_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "__sync_query",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            shd::table err;
            shd::object rows =
                run_statement(lua, db, sql, params, "query", &ok, &err);
            sqlite3_close(db);
            results.push_back(shd::make_object(lua, ok));
            if (ok) {
                results.push_back(rows);
            } else {
                results.push_back(err);
            }
            return results;
        });

    proxy.set_function(
        "__sync_query_one",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            shd::table err;
            shd::object row =
                run_statement(lua, db, sql, params, "query_one", &ok, &err);
            sqlite3_close(db);
            results.push_back(shd::make_object(lua, ok));
            if (ok) {
                results.push_back(row);  // row or nil
            } else {
                results.push_back(err);
            }
            return results;
        });

    proxy.set_function(
        "__sync_execute",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            shd::table err;
            shd::object res =
                run_statement(lua, db, sql, params, "execute", &ok, &err);
            sqlite3_close(db);
            results.push_back(shd::make_object(lua, ok));
            if (ok) {
                results.push_back(res);
            } else {
                results.push_back(err);
            }
            return results;
        });

    // __sync_transaction: the inline synchronous form. The async shim
    // orchestrates transactions in Lua over the __tx_* protocol and only
    // lands here when async is off (or the host lacks the primitives).
    proxy.set_function(
        "__sync_transaction",
        [inst](shd::this_state s,
               shd::protected_function callback) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }

            // BEGIN
            char* begin_err = nullptr;
            if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, &begin_err) !=
                SQLITE_OK) {
                std::string msg = begin_err ? begin_err : "BEGIN failed";
                sqlite3_free(begin_err);
                sqlite3_close(db);
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "db_query_failed", msg));
                return results;
            }

            // tx proxy shares this db handle.
            shd::table tx = make_handle_proxy(lua, db);
            shd::protected_function_result cb_res = callback(tx);
            bool commit = cb_res.valid();
            bool user_abort = false;

            if (cb_res.valid()) {
                // A boolean `false` first return is treated as user-initiated
                // rollback (matches the host facade's contract).
                shd::object first = cb_res.get(0);
                if (first.is<bool>() && !first.as<bool>()) {
                    commit = false;
                    user_abort = true;
                }
            }

            const char* tx_sql = commit ? "COMMIT" : "ROLLBACK";
            char* tx_err = nullptr;
            int trc = sqlite3_exec(db, tx_sql, nullptr, nullptr, &tx_err);
            std::string tx_msg = tx_err ? tx_err : "";
            sqlite3_free(tx_err);

            if (trc != SQLITE_OK) {
                sqlite3_close(db);
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, map_sqlite_error(trc),
                    tx_msg.empty()
                        ? std::string(tx_sql) + std::string(" failed")
                        : tx_msg));
                return results;
            }

            sqlite3_close(db);

            if (!cb_res.valid()) {
                // Lua callback threw — report as soft failure after rollback.
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua,
                                                   "transaction_rolled_back",
                                                   "callback raised an error"));
                return results;
            }

            if (user_abort) {
                // Callback explicitly returned false — propagate its returns.
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "transaction_rolled_back", "callback returned false"));
                return results;
            }

            // Success: forward the callback's return values.
            results.push_back(shd::make_object(lua, true));
            int n_returns = cb_res.return_count();
            for (int i = 0; i < n_returns; ++i) {
                results.push_back(cb_res.get<shd::object>(i));
            }
            return results;
        });

    // Async entry (docs/db-async-design.md M2): register the suspension via
    // the host primitive, then hand the task to the worker thread. Suspend
    // happens BEFORE enqueue so a fast worker completion always finds its
    // session (the host's yield-window guard handles the requeue). Returns
    // the session id, or 0 when the caller must run synchronously.
    proxy.set_function(
        "__db_submit",
        [inst](shd::this_state s, std::string method,
               std::optional<std::string> sql,
               std::optional<shd::table> params) -> uint64_t {
            if (!inst->async_enabled || inst->host_api == nullptr ||
                inst->host_api->lua_suspend_current == nullptr ||
                inst->host_api->lua_resume_session == nullptr) {
                return 0;
            }
            // The tx token arrives as the 4th Lua argument. the facade misbinds
            // this trailing integer (the bound parameter comes through
            // nil/0 even though the raw stack slot holds it), so read the
            // call's own frame directly.
            const uint64_t tx_token =
                (lua_gettop(s) >= 4 && lua_isinteger(s, 4))
                    ? static_cast<uint64_t>(lua_tointeger(s, 4))
                    : 0;
            sqlite_task task;
            task.kind = task_kind::stmt;
            const std::string tag = "db:sqlite:" + method;
            if (method == "__tx_begin") {
                task.kind = task_kind::tx_begin;
            } else if (method == "__tx_query" || method == "__tx_query_one" ||
                       method == "__tx_execute") {
                task.kind = task_kind::tx_stmt;
                task.method = method.substr(5);  // "__tx_" -> run mode
            } else if (method == "__tx_commit") {
                task.kind = task_kind::tx_commit;
            } else if (method == "__tx_rollback") {
                task.kind = task_kind::tx_rollback;
            } else {
                task.method = std::move(method);
            }
            task.tx_token = tx_token;
            if (task.kind == task_kind::stmt ||
                task.kind == task_kind::tx_stmt) {
                task.sql = sql ? std::move(*sql) : std::string();
            }
            // Positional params (Lua sequence 1..N), mirroring the sync
            // path's numeric-key bind. Type order matters: bool before int,
            // or sol folds booleans into integers.
            if (params && params->valid()) {
                for (const auto& kv : *params) {
                    if (kv.first.get_type() != shd::type::number) continue;
                    const shd::object& v = kv.second;
                    if (v.is<bool>()) {
                        task.params.push_back(v.as<bool>());
                    } else if (v.is<lua_Integer>()) {
                        task.params.push_back(
                            static_cast<int64_t>(v.as<lua_Integer>()));
                    } else if (v.is<double>()) {
                        task.params.push_back(v.as<double>());
                    } else if (v.is<std::string>()) {
                        task.params.push_back(v.as<std::string>());
                    } else {
                        task.params.push_back(nullptr);
                    }
                }
            }
            uint64_t session = inst->host_api->lua_suspend_current(
                inst->ctx, s, caller_timeout_ms(inst), tag.c_str());
            if (session == 0) {
                return 0;  // not suspendable here — caller runs sync
            }
            task.session = session;
            submit_async(inst, std::move(task));
            return session;
        });

    // Install the shim. Failure is impossible for static source, but a
    // fallback keeps the module usable synchronously if it ever trips.
    shd::load_result shim = lua.load(kAsyncShim, "=db_async_shim");
    bool shim_ok = shim.valid();
    if (shim_ok) {
        // load_result is not directly callable in shd: route through the
        // protected_function conversion so a shim error is a failed result,
        // not a C++ exception.
        shd::protected_function shim_fn = shim;
        shd::protected_function_result r = shim_fn(proxy, proxy["__db_submit"]);
        shim_ok = r.valid();
    }
    if (!shim_ok) {
        if (inst->host_api != nullptr && inst->host_api->log != nullptr) {
            inst->host_api->log(SHIELD_LOG_WARN, "database.sqlite",
                                inst->instance_id.c_str(),
                                "async shim failed to install; sync proxy");
        }
        for (const char* m : {"query", "query_one", "execute", "transaction"}) {
            std::string key = std::string("__sync_") + m;
            shd::object fn = proxy[key];
            proxy[m] = fn;
        }
    }

    return proxy;
}

// Proxy whose methods reuse a shared sqlite3* (used inside transactions).
shd::table make_handle_proxy(shd::state_view lua, sqlite3* db) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "query",
        [db](shd::this_state s, std::string sql,
             std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            shd::table err;
            shd::object rows =
                run_statement(lua, db, sql, params, "query", &ok, &err);
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? rows : shd::object(err));
            return results;
        });

    proxy.set_function(
        "query_one",
        [db](shd::this_state s, std::string sql,
             std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            shd::table err;
            shd::object row =
                run_statement(lua, db, sql, params, "query_one", &ok, &err);
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? row : shd::object(err));
            return results;
        });

    proxy.set_function(
        "execute",
        [db](shd::this_state s, std::string sql,
             std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            shd::table err;
            shd::object res =
                run_statement(lua, db, sql, params, "execute", &ok, &err);
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? res : shd::object(err));
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err) {
    // register_lua installs the shared, idempotent callable namespace
    // shield.database.sqlite. Lua passes a binding logical name; PluginHost
    // resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.sqlite: lua_State is null";
        }
        return 1;
    }
    shd::state_view lua(L);

    // Build the callable namespace shield.database.sqlite.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table database =
        shield::plugins::get_or_create_subtable(shield, "database");

    shd::object existing = database["sqlite"];
    if (!existing.is<shd::table>()) {
        auto* owner = reinterpret_cast<sqlite_instance*>(self);
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        mt.set_function(
            "__call",
            [host_api = owner ? owner->host_api : nullptr,
             ctx = owner ? owner->ctx : nullptr](
                shd::this_state s, shd::table /*self*/,
                std::optional<std::string> binding) -> shd::variadic_results {
                shd::state_view lua(s);
                shd::variadic_results results;
                std::string logical = binding.value_or("");
                auto* inst = shield::plugins::resolve_lua_binding(
                    host_api, ctx, logical, find_instance);
                if (!inst) {
                    shield::plugins::push_module_unavailable(results, lua,
                                                             logical);
                    return results;
                }
                shd::table proxy = make_instance_proxy(lua, inst);
                // Attach the shared mapper / register_mapper / entity DSL.
                // Failure here is non-fatal — the proxy keeps its C++ methods.
                shield::plugins::apply_db_mapper_api(lua, proxy);
                results.push_back(shd::make_object(lua, proxy));
                return results;
            });
        shield::plugins::set_metatable(ns, mt);
        database["sqlite"] = ns;
    }

    return 0;
}

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

const shield_pool_stats_v1& sqlite_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &sqlite_pool_get_stats,
    };
    return v;
}

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
