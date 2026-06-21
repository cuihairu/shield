// [SHIELD_DB_PLUGIN_SQLITE] SQLite backend plugin for shield_data.
//
// Implements the C ABI from shield/data/db_plugin.h on top of the
// sqlite3 C API. SQLite is a single-file embedded database: there is
// no server, host/port/user/password are ignored, and `database` is
// interpreted as a filesystem path (":memory:" for in-memory).
//
// This is intentionally a thin wrapper - we use sqlite3_prepare_v2
// with ? placeholders for parameters, sqlite3_bind_text for values,
// and materialise the full result set before returning. Transactions
// map directly to BEGIN/COMMIT/ROLLBACK SQL.

#include "shield/data/db_plugin.h"

#include <sqlite3.h>

#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

// Copy a sqlite3 column text (which may be NULL or static) into a
// freshly malloc'd buffer the host can later free(). Returns NULL
// when the SQL value was NULL.
char* dup_text_nullsafe(const unsigned char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(reinterpret_cast<const char*>(s));
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) {
        std::memcpy(out, s, len + 1);
    }
    return out;
}

// Map a sqlite3 result code to a stable shield error code.
const char* map_sqlite_error(int rc) {
    switch (rc) {
        case SQLITE_BUSY:   return "connection_timeout";
        case SQLITE_CONSTRAINT: return "constraint_violation";
        case SQLITE_MISMATCH:   return "syntax_error";
        case SQLITE_READONLY:
        case SQLITE_PERM:   return "auth_failed";
        case SQLITE_NOMEM:  return "pool_exhausted";
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB: return "db_query_failed";
        default:            return "db_query_failed";
    }
}

// Release all heap memory referenced by a result struct. Does not
// free `result` itself (host owns the struct).
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

// Copy a runtime string into freshly malloc'd UTF-8. nullptr -> nullptr.
char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

// Fill `out` with an error from `db` (sqlite3_errmsg). Always returns 0
// (soft failure) - the host will see success=0 and the error fields set.
int fill_sqlite_error(sqlite3* db, int rc, shield_db_result* out) {
    out->success = 0;
    const char* msg = sqlite3_errmsg(db);
    out->error_msg = dup_string(msg ? msg : "unknown sqlite error");
    out->error_code = dup_string(map_sqlite_error(rc));
    return 0;
}

// Execute a prepared statement with text-bound params. On success,
// fills `out` with rows (for SELECT) or affected count (for DML).
// Returns 0 on success or soft SQL failure, non-zero on hard error.
int run_prepared(sqlite3* db, const char* sql, const char* const* params,
                 int n_params, shield_db_result* out) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return fill_sqlite_error(db, rc, out);
    }

    // Bind text params. sqlite3 parameters are 1-indexed.
    for (int i = 0; i < n_params; ++i) {
        const char* p = params ? params[i] : nullptr;
        if (p) {
            rc = sqlite3_bind_text(stmt, i + 1, p, -1, SQLITE_TRANSIENT);
        } else {
            rc = sqlite3_bind_null(stmt, i + 1);
        }
        if (rc != SQLITE_OK) {
            int local = rc;
            sqlite3_finalize(stmt);
            return fill_sqlite_error(db, local, out);
        }
    }

    // Pull column count up front so empty result sets still report schema.
    int col_count = sqlite3_column_count(stmt);
    std::vector<const char*> cells;
    cells.reserve(static_cast<size_t>(col_count) * 4);  // small seed
    int row_count = 0;
    int64_t affected = 0;

    while (true) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            for (int c = 0; c < col_count; ++c) {
                const unsigned char* text = sqlite3_column_text(stmt, c);
                cells.push_back(dup_text_nullsafe(text));
            }
            ++row_count;
        } else if (rc == SQLITE_DONE) {
            affected = static_cast<int64_t>(sqlite3_changes(db));
            break;
        } else {
            // Hard error during step.
            sqlite3_finalize(stmt);
            int local = rc;
            // Free any partial rows before erroring.
            for (const char* cell : cells) {
                std::free(const_cast<char*>(cell));
            }
            return fill_sqlite_error(db, local, out);
        }
    }

    sqlite3_finalize(stmt);

    out->success = 1;
    out->error_msg = nullptr;
    out->error_code = nullptr;
    out->affected_rows = affected;
    // last_insert_id only meaningful for INSERT; expose unconditionally,
    // downstream code can ignore it.
    out->last_insert_id = static_cast<int64_t>(sqlite3_last_insert_rowid(db));
    out->row_count = row_count;
    out->col_count = col_count;
    out->cells = row_count > 0
                     ? static_cast<const char**>(
                         std::malloc(sizeof(char*) * cells.size()))
                     : nullptr;
    if (out->cells) {
        std::memcpy(const_cast<char**>(out->cells), cells.data(),
                    sizeof(char*) * cells.size());
    }
    return 0;
}

}  // namespace

struct shield_db_conn {
    sqlite3* db;
};

extern "C" {

SHIELD_DB_EXPORT
const shield_db_plugin* shield_db_plugin_api(void) {
    static const shield_db_plugin plugin = {
        SHIELD_DB_ABI_VERSION,
        "sqlite",
        "1.0.0",
        // connect
        [](const shield_db_connect_args* args,
           char* err_buf, int err_buf_size) -> shield_db_conn* {
            if (!args || !args->database) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size,
                                  "sqlite connect: missing database path");
                }
                return nullptr;
            }
            sqlite3* db = nullptr;
            // SQLITE_OPEN_URI lets users pass ":memory:" or file URIs.
            int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                        SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
            int rc = sqlite3_open_v2(args->database, &db, flags, nullptr);
            if (rc != SQLITE_OK) {
                if (err_buf && err_buf_size > 0) {
                    const char* msg = db ? sqlite3_errmsg(db)
                                         : "sqlite open failed";
                    std::snprintf(err_buf, err_buf_size, "%s", msg);
                }
                if (db) sqlite3_close(db);
                return nullptr;
            }
            // Short busy timeout so transient SQLITE_BUSY doesn't surface
            // as an error during normal pool churn.
            int timeout_ms = args->query_timeout_ms
                                 ? args->query_timeout_ms
                                 : 5000;
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
        [](shield_db_conn* c, const char* sql,
           const char* const* params, int n_params,
           shield_db_result* out) -> int {
            if (!c || !c->db || !sql) {
                out->success = 0;
                out->error_msg = dup_string("sqlite: invalid arguments");
                out->error_code = dup_string("db_query_failed");
                return 1;
            }
            return run_prepared(c->db, sql, params, n_params, out);
        },
        // execute (same path as query; SQLite doesn't distinguish)
        [](shield_db_conn* c, const char* sql,
           const char* const* params, int n_params,
           shield_db_result* out) -> int {
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
    return &plugin;
}

}  // extern "C"
