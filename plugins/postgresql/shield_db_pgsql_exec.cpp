// [SHIELD_PLUGIN] database.postgresql — C-ABI executor: result
// filling, SQLSTATE mapping, the $n placeholder-aware query runner
// and the shield_database_v1 glue.
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

// shield_db_conn is forward-declared opaque in database.h. The concrete
// layout must live at global scope so it completes the very type the vtable
// pointers reference — an anonymous-namespace definition would be a distinct
// type and the lambdas would not convert to the function pointers.
struct shield_db_conn {
    PGconn* pg;
};

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
}  // namespace

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

namespace {
int fill_from_pgresult(PGresult* res, bool is_select, shield_db_result* out) {
    ExecStatusType status = PQresultStatus(res);

    if (status == PGRES_COMMAND_OK) {
        out->success = 1;
        char* affected = PQcmdTuples(res);
        out->affected_rows =
            affected && affected[0] ? std::strtoll(affected, nullptr, 10) : 0;
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
                            const_cast<const char**>(out->cells)[idx++] =
                                nullptr;
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
    out->error_msg =
        dup_string(msg && msg[0] ? msg : "postgresql: query failed");
    const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    out->error_code = dup_string(map_sqlstate(sqlstate));
    return 0;
}

int run_pg_query(PGconn* conn, const char* sql, const char* const* params,
                 int n_params, bool is_select, shield_db_result* out) {
    if (!conn || !sql) {
        out->success = 0;
        out->error_msg = dup_string("postgresql: invalid arguments");
        out->error_code = dup_string("db_query_failed");
        return 1;
    }

    std::string rewritten = rewrite_placeholders(sql);

    PGresult* res;
    if (n_params > 0) {
        res =
            PQexecParams(conn, rewritten.c_str(), n_params, nullptr,
                         const_cast<const char**>(params), nullptr, nullptr, 0);
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
}  // namespace

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
        [](const shield_db_connect_args* args, char* err_buf,
           int err_buf_size) -> shield_db_conn* {
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
                // libpq's connect_timeout is whole seconds (an integer
                // string); a fractional value is a hard parse error that
                // fails the connect immediately. Round sub-second budgets
                // up to libpq's 1-second floor.
                const int secs = (args->connect_timeout_ms + 999) / 1000;
                std::snprintf(buf, sizeof(buf), "%d", secs);
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
        [](shield_db_conn* c, const char* sql, const char* const* params,
           int n_params, shield_db_result* out) -> int {
            if (!c || !c->pg) {
                out->success = 0;
                out->error_msg = dup_string("postgresql: connection is null");
                out->error_code = dup_string("connection_lost");
                return 1;
            }
            return run_pg_query(c->pg, sql, params, n_params, true, out);
        },
        // execute
        [](shield_db_conn* c, const char* sql, const char* const* params,
           int n_params, shield_db_result* out) -> int {
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
