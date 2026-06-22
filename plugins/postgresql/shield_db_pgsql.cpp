// [SHIELD_PLUGIN] database.postgresql — PostgreSQL provider for
// shield.database.v1.
//
// v1 ABI (shield_plugin_get_v1). libpq is a fairly thin layer over the wire
// protocol, so this plugin is small.
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

#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"

#include <libpq-fe.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
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
// v1 database vtable
// ---------------------------------------------------------------------------
const shield_database_v1& db_vtable() {
    static const shield_database_v1 v = {
        sizeof(shield_database_v1),
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
// v1 ABI entry
// ---------------------------------------------------------------------------
namespace {

struct pg_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
};

int pg_create(const struct shield_plugin_create_args_v1* args,
              struct shield_plugin_instance_v1** out,
              struct shield_error_v1* err) {
    (void)err;
    auto* inst = new pg_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](struct shield_plugin_instance_v1*,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_DATABASE_INTERFACE) == 0)
            return &db_vtable();
        return nullptr;
    };
    inst->shell.start = [](struct shield_plugin_instance_v1*, struct shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](struct shield_plugin_instance_v1* self) {
        delete reinterpret_cast<pg_instance*>(self);
    };
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
