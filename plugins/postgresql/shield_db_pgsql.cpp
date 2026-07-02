// [SHIELD_PLUGIN] database.postgresql — PostgreSQL provider for
// shield.database.v1.
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

#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"
#include "shield/plugin/host_api.h"
#include "shield_db_mapper.hpp"
#include "shield_lua_plugin_binding.hpp"

#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace {

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

void clear_result(shield_db_result* r) {
    if (!r) return;
    if (r->error_msg)  { std::free(const_cast<char*>(r->error_msg)); r->error_msg = nullptr; }
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

// Map a PG SQLSTATE (5-char code) to a stable shield error code.
// Reference: https://www.postgresql.org/docs/current/errcodes-appendix.html
const char* map_sqlstate(const char* sqlstate) {
    if (!sqlstate || sqlstate[0] == '\0') return "db_query_failed";
    if (sqlstate[0] == '0' && sqlstate[1] == '8') return "connection_lost";
    if (sqlstate[0] == '5' && sqlstate[1] == '3') return "pool_exhausted";
    if (sqlstate[0] == '4' && sqlstate[1] == '0') return "transaction_aborted";
    if (sqlstate[0] == '2' && sqlstate[1] == '3') return "constraint_violation";
    if (sqlstate[0] == '4' && sqlstate[1] == '2') return "syntax_error";
    if (sqlstate[0] == '2' && sqlstate[1] == '8') return "auth_failed";
    if (sqlstate[0] == '5' && sqlstate[1] == '7') return "connection_lost";
    return "db_query_failed";
}

int fill_from_pgresult(PGresult* res, bool is_select, shield_db_result* out) {
    ExecStatusType status = PQresultStatus(res);

    if (status == PGRES_COMMAND_OK) {
        out->success = 1;
        char* affected = PQcmdTuples(res);
        out->affected_rows = affected && affected[0]
                                 ? std::strtoll(affected, nullptr, 10)
                                 : 0;
        out->last_insert_id = 0;
        out->row_count = 0;
        out->col_count = 0;
        out->cells = nullptr;
        out->error_msg = nullptr;
        out->error_code = nullptr;
        return 0;
    }

    if (status == PGRES_TUPLES_OK && is_select) {
        int n_rows = PQntuples(res);
        int n_cols = PQnfields(res);
        out->success = 1;
        out->row_count = n_rows;
        out->col_count = n_cols;
        out->affected_rows = 0;
        out->last_insert_id = 0;
        out->error_msg = nullptr;
        out->error_code = nullptr;

        if (n_rows > 0 && n_cols > 0) {
            size_t cells_bytes = sizeof(char*) * static_cast<size_t>(n_rows) *
                                                   static_cast<size_t>(n_cols);
            out->cells = static_cast<const char**>(std::malloc(cells_bytes));
            if (out->cells) {
                int idx = 0;
                for (int r = 0; r < n_rows; ++r) {
                    for (int c = 0; c < n_cols; ++c) {
                        if (PQgetisnull(res, r, c)) {
                            const_cast<const char**>(out->cells)[idx++] = nullptr;
                        } else {
                            const char* v = PQgetvalue(res, r, c);
                            const_cast<const char**>(out->cells)[idx++] =
                                dup_string(v);
                        }
                    }
                }
            }
        } else {
            out->cells = nullptr;
        }
        return 0;
    }

    if (status == PGRES_TUPLES_OK && !is_select) {
        out->success = 1;
        out->row_count = 0;
        out->col_count = 0;
        out->affected_rows = 0;
        out->last_insert_id = 0;
        out->cells = nullptr;
        out->error_msg = nullptr;
        out->error_code = nullptr;
        return 0;
    }

    out->success = 0;
    out->affected_rows = 0;
    out->last_insert_id = 0;
    out->row_count = 0;
    out->col_count = 0;
    out->cells = nullptr;

    const char* msg = PQresultErrorMessage(res);
    out->error_msg = dup_string(msg && msg[0] ? msg : "postgresql: query failed");
    const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    out->error_code = dup_string(map_sqlstate(sqlstate));
    return 0;
}

// Rewrite ? placeholders to $N (libpq native form). Shared by both the C
// vtable and the Lua proxy. Naive about literals/comments — see file header.
std::string rewrite_placeholders(const char* sql) {
    std::string out;
    out.reserve(std::strlen(sql) + 8);
    int index = 0;
    for (const char* p = sql; *p; ++p) {
        if (*p == '?') {
            out.push_back('$');
            char buf[12];
            std::snprintf(buf, sizeof(buf), "%d", ++index);
            out += buf;
        } else {
            out.push_back(*p);
        }
    }
    return out;
}

int run_pg_query(PGconn* conn, const char* sql,
                 const char* const* params, int n_params,
                 bool is_select, shield_db_result* out) {
    if (!conn || !sql) {
        out->success = 0;
        out->error_msg = dup_string("postgresql: invalid arguments");
        out->error_code = dup_string("db_query_failed");
        return 1;
    }

    std::string rewritten = rewrite_placeholders(sql);

    PGresult* res;
    if (n_params > 0) {
        res = PQexecParams(conn,
                           rewritten.c_str(),
                           n_params,
                           nullptr,
                           const_cast<const char**>(params),
                           nullptr,
                           nullptr,
                           0);
    } else {
        res = PQexec(conn, rewritten.c_str());
    }

    int rc = fill_from_pgresult(res, is_select, out);
    PQclear(res);

    if (PQstatus(conn) == CONNECTION_BAD) {
        return 1;
    }
    return rc;
}

// shield_db_conn is forward-declared opaque in database.h; concrete layout
// defined here.
struct shield_db_conn {
    PGconn* pg;
};

// ---------------------------------------------------------------------------
// v1 database vtable (per-call connect; independent of the instance pool).
// ---------------------------------------------------------------------------
const shield_database_v1& db_vtable() {
    static const shield_database_v1 v = {
        sizeof(shield_database_v1),
        SHIELD_DATABASE_INTERFACE,
        "postgresql",
        "1.0.0",
        // connect
        [](const shield_db_connect_args* args,
           char* err_buf, int err_buf_size) -> shield_db_conn* {
            if (!args) return nullptr;
            std::string conninfo;
            auto append = [&](const char* key, const char* val) {
                if (val && val[0]) {
                    if (!conninfo.empty()) conninfo.push_back(' ');
                    conninfo += key;
                    conninfo.push_back('=');
                    conninfo += val;
                }
            };
            append("host", args->host);
            if (args->port > 0) {
                conninfo += " port=";
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", args->port);
                conninfo += buf;
            }
            append("user", args->user);
            append("password", args->password);
            append("dbname", args->database);

            if (args->connect_timeout_ms > 0) {
                char buf[16];
                double secs = args->connect_timeout_ms / 1000.0;
                std::snprintf(buf, sizeof(buf), "%.3f", secs);
                if (!conninfo.empty()) conninfo.push_back(' ');
                conninfo += "connect_timeout=";
                conninfo += buf;
            }

            PGconn* pg = PQconnectdb(conninfo.c_str());
            if (!pg) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size,
                                  "PQconnectdb returned NULL (out of memory)");
                return nullptr;
            }
            if (PQstatus(pg) != CONNECTION_OK) {
                const char* msg = PQerrorMessage(pg);
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size, "%s", msg);
                PQfinish(pg);
                return nullptr;
            }
            return new shield_db_conn{pg};
        },
        // disconnect
        [](shield_db_conn* c) {
            if (!c) return;
            if (c->pg) PQfinish(c->pg);
            delete c;
        },
        // ping
        [](shield_db_conn* c) -> int {
            if (!c || !c->pg) return 0;
            PGresult* res = PQexec(c->pg, "SELECT 1");
            int ok = (PQresultStatus(res) == PGRES_TUPLES_OK) ? 1 : 0;
            PQclear(res);
            return ok;
        },
        // query
        [](shield_db_conn* c, const char* sql,
           const char* const* params, int n_params,
           shield_db_result* out) -> int {
            if (!c || !c->pg) {
                out->success = 0;
                out->error_msg = dup_string("postgresql: connection is null");
                out->error_code = dup_string("connection_lost");
                return 1;
            }
            return run_pg_query(c->pg, sql, params, n_params, true, out);
        },
        // execute
        [](shield_db_conn* c, const char* sql,
           const char* const* params, int n_params,
           shield_db_result* out) -> int {
            if (!c || !c->pg) {
                out->success = 0;
                out->error_msg = dup_string("postgresql: connection is null");
                out->error_code = dup_string("connection_lost");
                return 1;
            }
            return run_pg_query(c->pg, sql, params, n_params, false, out);
        },
        // begin
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->pg) return 1;
            return run_pg_query(c->pg, "BEGIN", nullptr, 0, false, out);
        },
        // commit
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->pg) return 1;
            return run_pg_query(c->pg, "COMMIT", nullptr, 0, false, out);
        },
        // rollback
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->pg) return 1;
            return run_pg_query(c->pg, "ROLLBACK", nullptr, 0, false, out);
        },
        // free_result
        [](shield_db_result* r) { clear_result(r); },
    };
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json), owns a per-instance connection pool, and registers itself in
// a process-wide map so the Lua callable namespace can resolve plugins.bindings
// logical names to instances. The C++ vtable is also served through
// get_interface() for any C-ABI consumer that prefers the raw connect/query
// surface.
// ---------------------------------------------------------------------------
namespace {

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

    // Pool state. free_list holds healthy PGconn* ready for reuse.
    // current_size counts every connection we've ever handed out and not
    // yet PQfinish()'d (whether currently in free_list or checked out).
    std::mutex pool_mu;
    std::condition_variable pool_cv;
    std::queue<PGconn*> free_list;
    int current_size = 0;
};

// Process-wide registry: instance_id -> pgsql_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here.
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, pgsql_instance*>& instances_map() {
    static std::map<std::string, pgsql_instance*> m;
    return m;
}

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
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

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
        double secs = inst->connect_timeout_ms / 1000.0;
        std::snprintf(buf, sizeof(buf), "%.3f", secs);
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
            inst = o.inst; conn = o.conn; bad = o.bad;
            o.inst = nullptr; o.conn = nullptr;
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
    int timeout_ms = inst->connect_timeout_ms > 0
                         ? inst->connect_timeout_ms
                         : 5000;
    if (inst->pool_cv.wait_for(lk,
            std::chrono::milliseconds(timeout_ms),
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

// ---------------------------------------------------------------------------
// Lua helpers.
// ---------------------------------------------------------------------------

// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.database.* facade.
sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Build a Lua error table from a PGresult (pulls SQLSTATE + primary message).
sol::table make_error_from_result(sol::state_view lua, PGresult* res) {
    const char* sqlstate = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE)
                               : nullptr;
    const char* msg = res ? PQresultErrorMessage(res) : nullptr;
    return make_error_table(lua, map_sqlstate(sqlstate),
                            msg && msg[0] ? std::string(msg)
                                          : std::string("postgresql: query failed"));
}

// Convert the value of `res` at (row, col) into a Lua object. PostgreSQL
// returns everything as text via PQgetvalue; we rely on the field's OID
// (PQftype) only to distinguish "definitely integer" (INT2/INT4/INT8/OID)
// from "definitely float" (FLOAT4/FLOAT8/NUMERIC) from text. NULL stays nil.
sol::object pg_cell_to_lua(sol::state_view lua, PGresult* res,
                           int row, int col) {
    if (PQgetisnull(res, row, col)) return sol::nil;
    const char* v = PQgetvalue(res, row, col);
    if (!v) return sol::nil;
    Oid t = PQftype(res, col);
    std::string s = v;
    // Numeric types: INT2=21, INT4=23, INT8=20, OID=26. BOOL=16.
    if (t == 21 || t == 23 || t == 20 || t == 26) {
        try { return sol::make_object(lua, static_cast<lua_Integer>(
            std::strtoll(v, nullptr, 10))); }
        catch (...) { /* fall through */ }
    }
    if (t == 16) {
        // boolean — libpq renders 't'/'f'.
        return sol::make_object(lua, s == "t");
    }
    if (t == 700 || t == 701 || t == 1700) {
        try { return sol::make_object(lua, std::stod(s)); }
        catch (...) { /* fall through */ }
    }
    return sol::make_object(lua, s);
}

// Build a Lua row table from a PGresult at row `r`, keyed by column name.
sol::table pg_row_to_lua(sol::state_view lua, PGresult* res, int r) {
    auto row = lua.create_table();
    int n = PQnfields(res);
    for (int c = 0; c < n; ++c) {
        const char* name = PQfname(res, c);
        row[name ? name : "_"] = pg_cell_to_lua(lua, res, r, c);
    }
    return row;
}

// Collect positional params (Lua sequence table 1..N) as C strings. We bind
// everything as text — libpq parses them per-column via PQexecParams.
struct lua_params {
    std::vector<std::string> storage;
    std::vector<const char*> ptrs;  // points into storage
};

lua_params collect_lua_params(sol::optional<sol::table> params,
                              std::string* err_msg) {
    lua_params out;
    if (!params || !params->valid()) return out;
    // Iterate once, copying values so later binding can't invalidate them.
    std::vector<sol::object> positional;
    for (auto& kv : *params) {
        if (kv.first.get_type() != sol::type::number) continue;
        positional.push_back(kv.second);
    }
    out.storage.reserve(positional.size());
    out.ptrs.reserve(positional.size());
    for (size_t i = 0; i < positional.size(); ++i) {
        const sol::object& v = positional[i];
        if (!v.valid() || v == sol::nil) {
            out.storage.emplace_back();
            out.ptrs.push_back(nullptr);
        } else if (v.is<bool>()) {
            out.storage.push_back(v.as<bool>() ? "t" : "f");
            out.ptrs.push_back(out.storage.back().c_str());
        } else if (v.is<lua_Integer>()) {
            out.storage.push_back(std::to_string(v.as<lua_Integer>()));
            out.ptrs.push_back(out.storage.back().c_str());
        } else if (v.is<double>()) {
            out.storage.push_back(std::to_string(v.as<double>()));
            out.ptrs.push_back(out.storage.back().c_str());
        } else if (v.is<std::string>()) {
            out.storage.push_back(v.as<std::string>());
            out.ptrs.push_back(out.storage.back().c_str());
        } else {
            if (err_msg) *err_msg =
                "unsupported parameter type at index " + std::to_string(i + 1) +
                "; bound as NULL";
            out.storage.emplace_back();
            out.ptrs.push_back(nullptr);
        }
    }
    return out;
}

// Execute a statement on `conn` and return one of three Lua shapes.
//   "query"     -> sequence table {row1, row2, ...}
//   "query_one" -> single row table or nil
//   "execute"   -> table {affected=N}
// On error, *ok is set to false and *err_out receives an error table.
sol::object run_statement(sol::state_view lua, PGconn* conn,
                          const std::string& sql,
                          sol::optional<sol::table> params,
                          const char* mode,  // "query" | "query_one" | "execute"
                          bool* ok, sol::table* err_out,
                          bool* conn_bad) {
    std::string bind_err;
    lua_params lp = collect_lua_params(params, &bind_err);

    std::string rewritten = rewrite_placeholders(sql.c_str());

    PGresult* res = PQexecParams(conn,
                                 rewritten.c_str(),
                                 static_cast<int>(lp.ptrs.size()),
                                 nullptr,
                                 lp.ptrs.data(),
                                 nullptr,
                                 nullptr,
                                 0);
    if (!res) {
        // Out of memory on libpq side — conn is almost certainly toast.
        *ok = false;
        *conn_bad = true;
        *err_out = make_error_table(lua, "connection_lost",
                                    "postgresql: PQexecParams returned NULL");
        return sol::nil;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        *err_out = make_error_from_result(lua, res);
        if (PQstatus(conn) == CONNECTION_BAD) *conn_bad = true;
        PQclear(res);
        *ok = false;
        return sol::nil;
    }

    sol::object result = sol::nil;
    if (std::strcmp(mode, "execute") == 0) {
        char* affected = PQcmdTuples(res);
        auto t = lua.create_table();
        t["affected"] = affected && affected[0]
                            ? static_cast<lua_Integer>(
                                  std::strtoll(affected, nullptr, 10))
                            : 0;
        result = t;
    } else if (std::strcmp(mode, "query_one") == 0) {
        if (PQntuples(res) > 0) {
            result = pg_row_to_lua(lua, res, 0);
        } else {
            result = sol::nil;
        }
    } else {  // "query"
        auto rows = lua.create_table();
        int n = PQntuples(res);
        for (int r = 0; r < n; ++r) {
            rows[r + 1] = pg_row_to_lua(lua, res, r);  // Lua is 1-indexed
        }
        result = rows;
    }
    PQclear(res);
    *ok = true;
    return result;
}

// Forward decl: the transaction callback gets a handle-proxy bound to a
// single checked-out connection so all of its statements share the tx.
sol::table make_handle_proxy(sol::state_view lua, pgsql_instance* inst,
                             PGconn* conn, bool* conn_bad);

// Build a per-instance proxy table. Each method acquires from the pool,
// runs the statement, and returns the conn via RAII.
sol::table make_instance_proxy(sol::state_view lua, pgsql_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function("query",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", err));
                return results;
            }
            bool ok = false, conn_bad = false;
            sol::table err_t;
            sol::object rows = run_statement(lua, g.get(), sql, params,
                                             "query", &ok, &err_t, &conn_bad);
            g.bad = conn_bad;  // let pool_guard PQfinish dead conns
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? rows : err_t);
            return results;
        });

    proxy.set_function("query_one",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", err));
                return results;
            }
            bool ok = false, conn_bad = false;
            sol::table err_t;
            sol::object row = run_statement(lua, g.get(), sql, params,
                                            "query_one", &ok, &err_t, &conn_bad);
            g.bad = conn_bad;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? row : err_t);
            return results;
        });

    proxy.set_function("execute",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", err));
                return results;
            }
            bool ok = false, conn_bad = false;
            sol::table err_t;
            sol::object res = run_statement(lua, g.get(), sql, params,
                                            "execute", &ok, &err_t, &conn_bad);
            g.bad = conn_bad;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? res : err_t);
            return results;
        });

    proxy.set_function("transaction",
        [inst](sol::this_state s,
               sol::protected_function callback) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", err));
                return results;
            }

            // BEGIN
            bool conn_bad = false;
            {
                PGresult* res = PQexec(g.get(), "BEGIN");
                if (!res) {
                    conn_bad = true;
                } else {
                    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                        sol::table err_t = make_error_from_result(lua, res);
                        if (PQstatus(g.get()) == CONNECTION_BAD)
                            conn_bad = true;
                        PQclear(res);
                        g.bad = conn_bad;
                        results.push_back(sol::make_object(lua, false));
                        results.push_back(err_t);
                        return results;
                    }
                    PQclear(res);
                }
            }

            // Build the tx proxy sharing this conn.
            bool tx_conn_bad = false;
            sol::table tx = make_handle_proxy(lua, inst, g.get(), &tx_conn_bad);

            sol::protected_function_result cb_res = callback(tx);
            bool commit = cb_res.valid();
            bool user_abort = false;
            bool saw_error = false;

            if (cb_res.valid()) {
                // A boolean `false` first return is treated as user-initiated
                // rollback (matches the host facade's contract).
                sol::optional<bool> first = cb_res.get<sol::optional<bool>>(0);
                if (first && !*first) {
                    commit = false;
                    user_abort = true;
                }
            } else {
                // Lua callback threw — must rollback to clear aborted state.
                saw_error = true;
                commit = false;
            }
            // If the callback's own statements poisoned the connection or
            // marked conn_bad, we cannot safely COMMIT — force ROLLBACK.
            if (tx_conn_bad) {
                commit = false;
            }

            const char* tx_sql = commit ? "COMMIT" : "ROLLBACK";
            std::string tx_err_msg;
            const char* tx_err_code = "db_query_failed";
            bool tx_ok = false;
            {
                PGresult* res = PQexec(g.get(), tx_sql);
                if (!res) {
                    tx_err_msg = std::string(tx_sql) + " returned NULL";
                    conn_bad = true;
                } else {
                    if (PQresultStatus(res) == PGRES_COMMAND_OK) {
                        tx_ok = true;
                    } else {
                        const char* sqlstate = PQresultErrorField(
                            res, PG_DIAG_SQLSTATE);
                        tx_err_code = map_sqlstate(sqlstate);
                        const char* m = PQresultErrorMessage(res);
                        tx_err_msg = m && m[0] ? m
                                               : (std::string(tx_sql) +
                                                  " failed");
                        if (PQstatus(g.get()) == CONNECTION_BAD)
                            conn_bad = true;
                    }
                    PQclear(res);
                }
            }
            g.bad = conn_bad;

            if (!tx_ok) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, tx_err_code, tx_err_msg));
                return results;
            }

            if (saw_error) {
                // Callback raised — soft failure after successful rollback.
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "transaction_rolled_back",
                    "callback raised an error"));
                return results;
            }

            if (user_abort) {
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

// Proxy bound to a single checked-out connection (used inside transactions).
// Methods don't acquire from the pool — they run statements directly on
// `conn`. `*conn_bad` is shared with the transaction() lambda so the
// caller knows whether COMMIT is still safe.
sol::table make_handle_proxy(sol::state_view lua, pgsql_instance* /*inst*/,
                             PGconn* conn, bool* conn_bad) {
    auto proxy = lua.create_table();

    proxy.set_function("query",
        [conn, conn_bad](sol::this_state s, std::string sql,
                         sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false, cd = false;
            sol::table err_t;
            sol::object rows = run_statement(lua, conn, sql, params,
                                             "query", &ok, &err_t, &cd);
            if (cd) *conn_bad = true;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? rows : err_t);
            return results;
        });

    proxy.set_function("query_one",
        [conn, conn_bad](sol::this_state s, std::string sql,
                         sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false, cd = false;
            sol::table err_t;
            sol::object row = run_statement(lua, conn, sql, params,
                                            "query_one", &ok, &err_t, &cd);
            if (cd) *conn_bad = true;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? row : err_t);
            return results;
        });

    proxy.set_function("execute",
        [conn, conn_bad](sol::this_state s, std::string sql,
                         sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false, cd = false;
            sol::table err_t;
            sol::object res = run_statement(lua, conn, sql, params,
                                            "execute", &ok, &err_t, &cd);
            if (cd) *conn_bad = true;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? res : err_t);
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self,
                      struct lua_State* L,
                      shield_error_v1* err) {
    // register_lua installs the shared, idempotent callable namespace
    // shield.database.postgresql. Lua passes a binding logical name;
    // PluginHost resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.postgresql: lua_State is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    // Build the callable namespace shield.database.postgresql.
    auto shield = lua["shield"].get_or_create<sol::table>();
    auto database = shield["database"].get_or_create<sol::table>();

    sol::object existing = database["postgresql"];
    if (!existing.is<sol::table>()) {
        auto* owner = reinterpret_cast<pgsql_instance*>(self);
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        mt.set_function("__call",
            [host_api = owner ? owner->host_api : nullptr,
             ctx = owner ? owner->ctx : nullptr](
               sol::this_state s, sol::table /*self*/,
               sol::optional<std::string> binding) -> sol::variadic_results {
                sol::state_view lua(s);
                sol::variadic_results results;
                std::string logical = binding.value_or("");
                auto* inst = shield::plugins::resolve_lua_binding(
                    host_api, ctx, logical, find_instance);
                if (!inst) {
                    shield::plugins::push_module_unavailable(results, lua,
                                                             logical);
                    return results;
                }
                sol::table proxy = make_instance_proxy(lua, inst);
                shield::plugins::apply_db_mapper_api(lua, proxy);
                results.push_back(sol::make_object(lua, proxy));
                return results;
            });
        ns[sol::metatable_key] = mt;
        database["postgresql"] = ns;
    }

    return 0;
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

int pg_create(const shield_plugin_create_args_v1* args,
              shield_plugin_instance_v1** out,
              shield_error_v1* err) {
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
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        // shell is the first member of pgsql_instance (offset 0), so self
        // points at the enclosing pgsql_instance. Standard C-ABI pattern.
        auto* inst = reinterpret_cast<pgsql_instance*>(self);
        unregister_instance(inst->instance_id);
        drain_pool(inst);
        delete inst;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.postgresql",
        "1.0.0",
        pg_create,
    };
    return &abi;
}
