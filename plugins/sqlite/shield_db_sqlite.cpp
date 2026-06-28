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

#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"
#include "shield/plugin/host_api.h"
#include "shield_db_mapper.hpp"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <sqlite3.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
        case SQLITE_BUSY:        return "connection_timeout";
        case SQLITE_CONSTRAINT:  return "constraint_violation";
        case SQLITE_MISMATCH:    return "syntax_error";
        case SQLITE_READONLY:
        case SQLITE_PERM:        return "auth_failed";
        case SQLITE_NOMEM:       return "pool_exhausted";
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:      return "db_query_failed";
        default:                 return "db_query_failed";
    }
}

// Release heap memory referenced by a result struct. Does not free `result`
// itself (host owns the struct).
void clear_result(shield_db_result* r) {
    if (!r) return;
    if (r->error_msg)  { std::free(const_cast<char*>(r->error_msg));  r->error_msg = nullptr; }
    if (r->error_code) { std::free(const_cast<char*>(r->error_code)); r->error_code = nullptr; }
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
                cells.push_back(dup_text_nullsafe(sqlite3_column_text(stmt, c)));
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
    out->cells = row_count > 0
                     ? static_cast<const char**>(std::malloc(sizeof(char*) * cells.size()))
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
        [](const shield_db_connect_args* args, char* err_buf, int err_buf_size) -> shield_db_conn* {
            if (!args || !args->database) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size, "sqlite connect: missing database path");
                return nullptr;
            }
            sqlite3* db = nullptr;
            int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                        SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
            int rc = sqlite3_open_v2(args->database, &db, flags, nullptr);
            if (rc != SQLITE_OK) {
                if (err_buf && err_buf_size > 0) {
                    const char* msg = db ? sqlite3_errmsg(db) : "sqlite open failed";
                    std::snprintf(err_buf, err_buf_size, "%s", msg);
                }
                if (db) sqlite3_close(db);
                return nullptr;
            }
            int timeout_ms = args->query_timeout_ms ? args->query_timeout_ms : 5000;
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
// config_json) and registers itself in a process-wide map so the Lua
// callable namespace `shield.database.sqlite(binding)` can resolve it.
// The C++ vtable is still served through get_interface() for any C-ABI
// caller (none in tree today, but kept for forward compatibility).
// ---------------------------------------------------------------------------
namespace {

struct sqlite_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string database_path = ":memory:";  // from config "database"
    int query_timeout_ms = 5000;             // from config "query_timeout_ms"
};

// Process-wide registry: instance_id -> sqlite_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Map is read on every proxy creation, so it must be
// thread-safe.
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, sqlite_instance*>& instances_map() {
    static std::map<std::string, sqlite_instance*> m;
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
        if (j.contains("query_timeout_ms") && j["query_timeout_ms"].is_number_integer()) {
            inst->query_timeout_ms = j["query_timeout_ms"].get<int>();
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
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
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
sol::table make_error_table(sol::state_view lua, const char* code,
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
int bind_lua_param(sqlite3_stmt* stmt, int idx, const sol::object& v,
                   std::string* err_msg) {
    if (!v.valid() || v == sol::nil) {
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
                                 static_cast<int>(s.size()),
                                 SQLITE_TRANSIENT);
    }
    // Fallback: bind NULL for anything we can't convert (tables, functions,
    // userdata). This avoids silent mis-binding; caller sees a soft warning.
    if (err_msg) *err_msg = "unsupported parameter type at index " +
                            std::to_string(idx) + "; bound as NULL";
    return sqlite3_bind_null(stmt, idx);
}

// Convert the current row of `stmt` into a Lua table keyed by column name.
sol::table row_to_lua(sol::state_view lua, sqlite3_stmt* stmt) {
    auto row = lua.create_table();
    int n = sqlite3_column_count(stmt);
    for (int c = 0; c < n; ++c) {
        const char* name = sqlite3_column_name(stmt, c);
        switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_INTEGER:
                row[name] = static_cast<lua_Integer>(sqlite3_column_int64(stmt, c));
                break;
            case SQLITE_FLOAT:
                row[name] = sqlite3_column_double(stmt, c);
                break;
            case SQLITE_TEXT: {
                const unsigned char* t = sqlite3_column_text(stmt, c);
                row[name] = t ? std::string(reinterpret_cast<const char*>(t))
                              : sol::nil;
                break;
            }
            case SQLITE_BLOB: {
                const void* b = sqlite3_column_blob(stmt, c);
                int sz = sqlite3_column_bytes(stmt, c);
                row[name] = b ? std::string(static_cast<const char*>(b),
                                            static_cast<size_t>(sz))
                              : sol::nil;
                break;
            }
            default:
                row[name] = sol::nil;  // SQLITE_NULL
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
sol::object run_statement(sol::state_view lua, sqlite3* db,
                          const std::string& sql,
                          sol::optional<sol::table> params,
                          const char* mode,  // "query" | "query_one" | "execute"
                          bool* ok, sol::table* err_out) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        *ok = false;
        *err_out = make_error_table(lua, map_sqlite_error(rc),
                                    sqlite3_errmsg(db));
        return sol::nil;
    }
    // Bind parameters (Lua sequence table 1..N). Collect positional values
    // first to avoid any iterator invalidation from stack work during bind.
    if (params && params->valid()) {
        std::vector<sol::object> positional;
        for (auto& kv : *params) {
            if (kv.first.get_type() != sol::type::number) continue;
            positional.push_back(kv.second);
        }
        for (size_t i = 0; i < positional.size(); ++i) {
            std::string bind_err;
            int brc = bind_lua_param(stmt, static_cast<int>(i + 1),
                                     positional[i], &bind_err);
            if (brc != SQLITE_OK) {
                sqlite3_finalize(stmt);
                *ok = false;
                *err_out = make_error_table(lua, map_sqlite_error(brc),
                                            bind_err.empty()
                                                ? sqlite3_errmsg(db)
                                                : bind_err);
                return sol::nil;
            }
        }
    }

    sol::object result = sol::nil;
    if (std::strcmp(mode, "execute") == 0) {
        // Step once; we don't collect rows for DML.
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            *ok = false;
            *err_out = make_error_table(lua, map_sqlite_error(rc),
                                        sqlite3_errmsg(db));
            return sol::nil;
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
            result = sol::nil;  // no rows
        } else {
            sqlite3_finalize(stmt);
            *ok = false;
            *err_out = make_error_table(lua, map_sqlite_error(rc),
                                        sqlite3_errmsg(db));
            return sol::nil;
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
                return sol::nil;
            }
        }
        result = rows;
    }
    sqlite3_finalize(stmt);
    *ok = true;
    return result;
}

// Build a per-call proxy table bound to a specific sqlite3* handle. Used by
// transaction() so the callback's tx:execute/tx:query share one connection.
sol::table make_handle_proxy(sol::state_view lua, sqlite3* db);

// Build a per-instance proxy table that opens a fresh connection per call.
sol::table make_instance_proxy(sol::state_view lua, sqlite_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function("query",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            sol::table err;
            sol::object rows = run_statement(lua, db, sql, params,
                                             "query", &ok, &err);
            sqlite3_close(db);
            results.push_back(sol::make_object(lua, ok));
            if (ok) {
                results.push_back(rows);
            } else {
                results.push_back(err);
            }
            return results;
        });

    proxy.set_function("query_one",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            sol::table err;
            sol::object row = run_statement(lua, db, sql, params,
                                            "query_one", &ok, &err);
            sqlite3_close(db);
            results.push_back(sol::make_object(lua, ok));
            if (ok) {
                results.push_back(row);  // row or nil
            } else {
                results.push_back(err);
            }
            return results;
        });

    proxy.set_function("execute",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            sol::table err;
            sol::object res = run_statement(lua, db, sql, params,
                                            "execute", &ok, &err);
            sqlite3_close(db);
            results.push_back(sol::make_object(lua, ok));
            if (ok) {
                results.push_back(res);
            } else {
                results.push_back(err);
            }
            return results;
        });

    proxy.set_function("transaction",
        [inst](sol::this_state s,
               sol::protected_function callback) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", open_err));
                return results;
            }

            // BEGIN
            char* begin_err = nullptr;
            if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, &begin_err) != SQLITE_OK) {
                std::string msg = begin_err ? begin_err : "BEGIN failed";
                sqlite3_free(begin_err);
                sqlite3_close(db);
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "db_query_failed", msg));
                return results;
            }

            // tx proxy shares this db handle.
            sol::table tx = make_handle_proxy(lua, db);
            sol::protected_function_result cb_res = callback(tx);
            bool commit = cb_res.valid();
            bool user_abort = false;

            if (cb_res.valid()) {
                // A boolean `false` first return is treated as user-initiated
                // rollback (matches the host facade's contract).
                sol::optional<bool> first = cb_res.get<sol::optional<bool>>(0);
                if (first && !*first) {
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
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, map_sqlite_error(trc),
                    tx_msg.empty() ? std::string(tx_sql) +
                                     std::string(" failed") : tx_msg));
                return results;
            }

            sqlite3_close(db);

            if (!cb_res.valid()) {
                // Lua callback threw — report as soft failure after rollback.
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "transaction_rolled_back",
                    "callback raised an error"));
                return results;
            }

            if (user_abort) {
                // Callback explicitly returned false — propagate its returns.
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "transaction_rolled_back",
                    "callback returned false"));
                return results;
            }

            // Success: forward the callback's return values.
            results.push_back(sol::make_object(lua, true));
            int n_returns = cb_res.return_count();
            for (int i = 0; i < n_returns; ++i) {
                results.push_back(cb_res.get<sol::object>(i));
            }
            return results;
        });

    return proxy;
}

// Proxy whose methods reuse a shared sqlite3* (used inside transactions).
sol::table make_handle_proxy(sol::state_view lua, sqlite3* db) {
    auto proxy = lua.create_table();

    proxy.set_function("query",
        [db](sol::this_state s, std::string sql,
             sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            sol::table err;
            sol::object rows = run_statement(lua, db, sql, params,
                                             "query", &ok, &err);
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? rows : err);
            return results;
        });

    proxy.set_function("query_one",
        [db](sol::this_state s, std::string sql,
             sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            sol::table err;
            sol::object row = run_statement(lua, db, sql, params,
                                            "query_one", &ok, &err);
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? row : err);
            return results;
        });

    proxy.set_function("execute",
        [db](sol::this_state s, std::string sql,
             sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            sol::table err;
            sol::object res = run_statement(lua, db, sql, params,
                                            "execute", &ok, &err);
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? res : err);
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self,
                      struct lua_State* L,
                      shield_error_v1* err) {
    // register_lua installs the shared, idempotent callable namespace
    // shield.database.sqlite. Lua passes a binding logical name; host config
    // resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.sqlite: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<sqlite_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.sqlite: host binding resolver is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    // Build the callable namespace shield.database.sqlite.
    auto shield = lua["shield"].get_or_create<sol::table>();
    auto database = shield["database"].get_or_create<sol::table>();

    sol::object existing = database["sqlite"];
    if (!existing.is<sol::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function("__call",
            [host_api, ctx](sol::this_state s, sol::table /*self*/,
               std::string binding) -> sol::object {
                sol::state_view lua(s);
                const char* instance_id =
                    host_api->binding_instance_id(ctx, binding.c_str());
                if (!instance_id) return sol::nil;
                auto* inst = find_instance(instance_id);
                if (!inst) return sol::nil;
                sol::table proxy = make_instance_proxy(lua, inst);
                // Attach the shared mapper / register_mapper / entity DSL.
                // Failure here is non-fatal — the proxy keeps its C++ methods.
                shield::plugins::apply_db_mapper_api(lua, proxy);
                return sol::make_object(lua, proxy);
            });
        ns[sol::metatable_key] = mt;
        database["sqlite"] = ns;
    }

    return 0;
}

int sqlite_create(const shield_plugin_create_args_v1* args,
                  shield_plugin_instance_v1** out,
                  shield_error_v1* err) {
    (void)err;
    auto* inst = new sqlite_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::string(iface) == SHIELD_DATABASE_INTERFACE)
            return &db_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        // shell is the first member of sqlite_instance (offset 0), so self
        // points at the enclosing sqlite_instance. Standard C-ABI pattern.
        auto* inst = reinterpret_cast<sqlite_instance*>(self);
        unregister_instance(inst->instance_id);
        delete inst;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.sqlite",
        "1.0.0",
        sqlite_create,
    };
    return &abi;
}
