// [SHIELD_PLUGIN] database.sqlite — SQLite provider for shield.database.v1.
//
// Implements the new v1 ABI (shield_plugin_get_v1) on top of the sqlite3 C
// API. SQLite is a single-file embedded database: host/port/user/password are
// ignored, and `database` (passed via shield_db_connect_args at connect time)
// is interpreted as a filesystem path (":memory:" for in-memory).
//
// The SQL surface (connect/disconnect/ping/query/execute/begin/commit/
// rollback/free_result) is inherited verbatim from the legacy shield_db_plugin
// so DatabasePool adapts by renaming the type only.

#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"

#include <sqlite3.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
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
// v1 ABI entry. The instance is a thin shell — get_interface returns the
// shared static db_vtable(); start/shutdown are trivial because sqlite
// connections are created per-connect and carry no instance-global state.
// ---------------------------------------------------------------------------
namespace {

struct sqlite_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
};

int sqlite_create(const shield_plugin_create_args_v1* args,
                  shield_plugin_instance_v1** out,
                  shield_error_v1* err) {
    (void)err;
    auto* inst = new sqlite_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
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
        delete reinterpret_cast<sqlite_instance*>(self);
    };
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
