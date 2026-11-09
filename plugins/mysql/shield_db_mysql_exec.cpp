// [SHIELD_PLUGIN] database.mysql — statement executor: prepared-
// statement binding (mysql_stmt_*), typed result collection, error
// mapping and the C-ABI connect/query vtable glue.
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

// shield_db_conn is forward-declared opaque in database.h. The concrete
// layout must live at global scope so it completes the very type the vtable
// pointers reference — an anonymous-namespace definition would be a distinct
// type and the lambdas would not convert to the function pointers.
struct shield_db_conn {
    MYSQL* db;
};

namespace {
char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}
}  // namespace

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

// Map a MySQL/MariaDB error to a stable shield error code. Prefers the
// numeric error (server errno for server-side errors, CR_* codes 2000+ for
// client-side ones) and falls back to message sniffing for anything the
// table doesn't cover.
const char* map_mysql_error(unsigned err, const char* msg) {
    switch (err) {
        case CR_CONN_HOST_ERROR:
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_LOST:
        case CR_SERVER_LOST_EXTENDED:  // 2055 — extended read-failure variant
            return "connection_lost";
        case 1044:
        case 1045:
        case 1698:  // "Access denied" family
            return "auth_failed";
        case 1064:  // ER_PARSE_ERROR
        case 1149:  // ER_SYNTAX_ERROR
            return "syntax_error";
        case 1022:
        case 1062:  // ER_DUP_ENTRY
        case 1451:
        case 1452:  // FK violations
            return "constraint_violation";
        case 1205:  // ER_LOCK_WAIT_TIMEOUT
            return "connection_timeout";
        case 1213:  // ER_LOCK_DEADLOCK
            return "transaction_aborted";
        default:
            break;
    }
    if (msg) {
        std::string m(msg);
        if (m.find("Lost connection") != std::string::npos ||
            m.find("server has gone away") != std::string::npos ||
            m.find("Can't connect") != std::string::npos)
            return "connection_lost";
        if (m.find("timed out") != std::string::npos ||
            m.find("timeout") != std::string::npos)
            return "connection_timeout";
        if (m.find("Duplicate") != std::string::npos ||
            m.find("foreign key") != std::string::npos ||
            m.find("constraint") != std::string::npos)
            return "constraint_violation";
        if (m.find("Deadlock") != std::string::npos)
            return "transaction_aborted";
    }
    return "db_query_failed";
}

// Fill `out` with the last error on `mysql`. Always returns 0 (soft failure:
// the statement failed, not the vtable call).
int fill_mysql_error(MYSQL* mysql, shield_db_result* out) {
    out->success = 0;
    unsigned err = mysql ? mysql_errno(mysql) : 0;
    const char* msg = mysql ? mysql_error(mysql) : "mysql: connection is null";
    if (!msg || !msg[0]) msg = "unknown mysql error";
    out->error_msg = dup_string(msg);
    out->error_code = dup_string(map_mysql_error(err, msg));
    return 0;
}

int fill_text_error(const char* msg, shield_db_result* out) {
    out->success = 0;
    out->error_msg = dup_string(msg);
    out->error_code = dup_string("db_query_failed");
    return 0;
}

// True when the column should be received into an int64 buffer.
bool col_is_int(const MYSQL_FIELD& f) {
    switch (f.type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_YEAR:
            return true;
        default:
            return false;
    }
}

// True when the column should be received into a double buffer.
bool col_is_double(const MYSQL_FIELD& f) {
    switch (f.type) {
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return true;
        default:
            return false;
    }
}

// Execute a prepared statement and fetch the complete result set (if any)
// into `res`. On failure fills *err and returns false.
bool exec_typed(MYSQL* mysql, const char* sql,
                const std::vector<MYSQL_BIND>& params, stmt_result* res,
                stmt_error* err) {
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) {
        *err = {"db_query_failed", "mysql_stmt_init failed"};
        return false;
    }

    // Ask the driver to update field->max_length during store_result so the
    // string buffers can be sized correctly before the fetch binds.
    my_bool update_max_len = 1;
    mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_len);

    bool ok = false;
    if (mysql_stmt_prepare(stmt, sql,
                           static_cast<unsigned long>(strlen(sql))) != 0) {
        *err = {map_mysql_error(mysql_stmt_errno(stmt), mysql_stmt_error(stmt)),
                mysql_stmt_error(stmt)};
    } else if (mysql_stmt_param_count(stmt) !=
               static_cast<unsigned long>(params.size())) {
        *err = {"db_query_failed",
                "mysql: parameter count mismatch (" +
                    std::to_string(params.size()) + " given, " +
                    std::to_string(mysql_stmt_param_count(stmt)) +
                    " expected)"};
    } else {
        if (!params.empty() &&
            mysql_stmt_bind_param(
                stmt, const_cast<MYSQL_BIND*>(params.data())) != 0) {
            *err = {
                map_mysql_error(mysql_stmt_errno(stmt), mysql_stmt_error(stmt)),
                mysql_stmt_error(stmt)};
        } else if (mysql_stmt_execute(stmt) != 0) {
            *err = {
                map_mysql_error(mysql_stmt_errno(stmt), mysql_stmt_error(stmt)),
                mysql_stmt_error(stmt)};
        } else {
            ok = true;
        }
    }

    if (!ok) {
        mysql_stmt_close(stmt);
        return false;
    }

    // No result set (DML / SET / ...): report affected rows + insert id.
    if (mysql_stmt_field_count(stmt) == 0) {
        res->affected = static_cast<int64_t>(mysql_stmt_affected_rows(stmt));
        res->insert_id = static_cast<int64_t>(mysql_stmt_insert_id(stmt));
        mysql_stmt_close(stmt);
        return true;
    }

    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (!meta) {
        *err = {"db_query_failed", "mysql_stmt_result_metadata failed"};
        mysql_stmt_close(stmt);
        return false;
    }
    unsigned n_fields = mysql_num_fields(meta);
    res->col_count = static_cast<int>(n_fields);
    res->has_rows = true;

    // Materialize the result so max_length becomes authoritative.
    if (mysql_stmt_store_result(stmt) != 0) {
        *err = {map_mysql_error(mysql_stmt_errno(stmt), mysql_stmt_error(stmt)),
                mysql_stmt_error(stmt)};
        mysql_free_result(meta);
        mysql_stmt_close(stmt);
        return false;
    }

    // Column metadata (a copy — `meta` is freed before the fetch loop).
    std::vector<MYSQL_FIELD> fields(mysql_fetch_fields(meta),
                                    mysql_fetch_fields(meta) + n_fields);
    std::vector<unsigned char> kind(n_fields);
    std::vector<unsigned long> buf_size(n_fields);
    for (unsigned c = 0; c < n_fields; ++c) {
        if (col_is_int(fields[c])) {
            kind[c] = kCellInt;
            buf_size[c] = sizeof(int64_t);
        } else if (col_is_double(fields[c])) {
            kind[c] = kCellDbl;
            buf_size[c] = sizeof(double);
        } else {
            kind[c] = kCellStr;
            buf_size[c] = fields[c].max_length + 1;
        }
        res->col_names.push_back(fields[c].name ? fields[c].name
                                                : std::to_string(c + 1));
    }
    mysql_free_result(meta);

    res->row_count = static_cast<int>(mysql_stmt_num_rows(stmt));
    res->ints.assign(res->row_count, std::vector<int64_t>(n_fields, 0));
    res->dbls.assign(res->row_count, std::vector<double>(n_fields, 0.0));
    res->strs.assign(res->row_count, std::vector<std::string>(n_fields));
    res->kinds.assign(res->row_count, kind);

    // Per-column fetch buffers: one slot per column, reused across rows.
    std::vector<MYSQL_BIND> bind(n_fields, MYSQL_BIND{});
    std::vector<int64_t> int_buf(n_fields, 0);
    std::vector<double> dbl_buf(n_fields, 0.0);
    std::vector<std::string> str_buf(n_fields);
    std::vector<unsigned long> str_len(n_fields, 0);
    std::vector<my_bool> is_null(n_fields, 0);
    std::vector<my_bool> trunc(n_fields, 0);
    for (unsigned c = 0; c < n_fields; ++c) {
        bind[c].is_null = &is_null[c];
        bind[c].error = &trunc[c];
        if (kind[c] == kCellInt) {
            bind[c].buffer_type = MYSQL_TYPE_LONGLONG;
            bind[c].buffer = &int_buf[c];
            bind[c].buffer_length = sizeof(int64_t);
        } else if (kind[c] == kCellDbl) {
            bind[c].buffer_type = MYSQL_TYPE_DOUBLE;
            bind[c].buffer = &dbl_buf[c];
            bind[c].buffer_length = sizeof(double);
        } else {
            str_buf[c].assign(buf_size[c], '\0');
            bind[c].buffer_type = MYSQL_TYPE_STRING;
            bind[c].buffer = str_buf[c].data();
            bind[c].buffer_length = buf_size[c];
            bind[c].length = &str_len[c];
        }
    }
    if (mysql_stmt_bind_result(stmt, bind.data()) != 0) {
        *err = {map_mysql_error(mysql_stmt_errno(stmt), mysql_stmt_error(stmt)),
                mysql_stmt_error(stmt)};
        mysql_stmt_close(stmt);
        return false;
    }

    for (int r = 0; r < res->row_count; ++r) {
        int fetch_rc = mysql_stmt_fetch(stmt);
        if (fetch_rc == MYSQL_NO_DATA) break;
        if (fetch_rc != 0 && fetch_rc != MYSQL_DATA_TRUNCATED) {
            *err = {
                map_mysql_error(mysql_stmt_errno(stmt), mysql_stmt_error(stmt)),
                mysql_stmt_error(stmt)};
            mysql_stmt_close(stmt);
            return false;
        }
        for (unsigned c = 0; c < n_fields; ++c) {
            if (is_null[c]) {
                res->kinds[r][c] = kCellNull;
                continue;
            }
            switch (res->kinds[r][c]) {
                case kCellInt:
                    res->ints[r][c] = int_buf[c];
                    break;
                case kCellDbl:
                    res->dbls[r][c] = dbl_buf[c];
                    break;
                default:
                    res->strs[r][c].assign(str_buf[c].data(),
                                           str_len[c] ? str_len[c] : 0);
                    break;
            }
        }
    }

    mysql_stmt_close(stmt);
    return true;
}

int run_stmt(MYSQL* mysql, const char* sql, const char* const* params,
             int n_params, bool collect_rows, shield_db_result* out) {
    if (!mysql || !sql) {
        return fill_text_error("mysql: invalid arguments", out);
    }

    // vtable params are all text — bind them as strings.
    std::vector<MYSQL_BIND> binds(n_params, MYSQL_BIND{});
    std::vector<my_bool> nulls(n_params, 0);
    std::vector<unsigned long> lengths(n_params, 0);
    for (int i = 0; i < n_params; ++i) {
        if (!params || !params[i]) {
            nulls[i] = 1;
            binds[i].buffer_type = MYSQL_TYPE_NULL;
            binds[i].is_null = &nulls[i];
        } else {
            lengths[i] = static_cast<unsigned long>(strlen(params[i]));
            binds[i].buffer_type = MYSQL_TYPE_STRING;
            binds[i].buffer = const_cast<char*>(params[i]);
            binds[i].buffer_length = lengths[i];
            binds[i].length = &lengths[i];
        }
    }

    stmt_result res;
    stmt_error err;
    if (!exec_typed(mysql, sql, binds, &res, &err)) {
        out->success = 0;
        out->error_msg = dup_string(err.msg.c_str());
        out->error_code = dup_string(err.code.c_str());
        return 0;
    }

    out->success = 1;
    out->error_msg = nullptr;
    out->error_code = nullptr;
    out->affected_rows = res.affected;
    out->last_insert_id = res.insert_id;
    out->row_count = 0;
    out->col_count = 0;
    out->cells = nullptr;

    if (!collect_rows || !res.has_rows) return 0;

    // The C-ABI surface carries text cells: format each typed cell.
    out->row_count = res.row_count;
    out->col_count = res.col_count;
    size_t total =
        static_cast<size_t>(res.row_count) * static_cast<size_t>(res.col_count);
    out->cells = static_cast<const char**>(
        std::malloc(sizeof(char*) * (total ? total : 1)));
    if (!out->cells) return fill_text_error("mysql: out of memory", out);

    char num_buf[32];
    size_t k = 0;
    for (int r = 0; r < res.row_count; ++r) {
        for (int c = 0; c < res.col_count; ++c, ++k) {
            switch (res.kinds[r][c]) {
                case kCellNull:
                    out->cells[k] = nullptr;
                    break;
                case kCellInt:
                    std::snprintf(num_buf, sizeof(num_buf), "%lld",
                                  static_cast<long long>(res.ints[r][c]));
                    out->cells[k] = dup_string(num_buf);
                    break;
                case kCellDbl:
                    std::snprintf(num_buf, sizeof(num_buf), "%.17g",
                                  res.dbls[r][c]);
                    out->cells[k] = dup_string(num_buf);
                    break;
                default:
                    out->cells[k] = dup_string(res.strs[r][c].c_str());
                    break;
            }
        }
    }
    return 0;
}

int run_simple(MYSQL* mysql, const char* sql, shield_db_result* out) {
    return run_stmt(mysql, sql, nullptr, 0, false, out);
}

// ---------------------------------------------------------------------------
// v1 database vtable (C ABI — retained for out-of-tree C-ABI consumers)
//
// NOTE: The C vtable does NOT use the per-instance pool. Each connect builds
// a fresh Session and disconnect closes it. This preserves the existing
// vtable semantics. Only Lua callers (via shield.database.mysql proxy) get
// pooled connections.
// ---------------------------------------------------------------------------
const shield_database_v1& db_vtable() {
    static const shield_database_v1 v = {
        sizeof(shield_database_v1),
        SHIELD_DATABASE_INTERFACE,
        "mysql",
        "1.0.0",
        // connect
        [](const shield_db_connect_args* args, char* err_buf,
           int err_buf_size) -> shield_db_conn* {
            if (!args) return nullptr;
            MYSQL* db = mysql_init(nullptr);
            if (!db) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size,
                                  "mysql: out of memory");
                return nullptr;
            }
            unsigned int timeout =
                args->connect_timeout_ms
                    ? std::max(1u, static_cast<unsigned>(
                                       args->connect_timeout_ms / 1000))
                    : 5;
            mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
            mysql_options(db, MYSQL_SET_CHARSET_NAME, "utf8mb4");
            if (!mysql_real_connect(db, args->host ? args->host : "localhost",
                                    args->user ? args->user : "root",
                                    args->password ? args->password : "",
                                    args->database ? args->database : "shield",
                                    args->port ? args->port : 3306, nullptr,
                                    0)) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size, "%s", mysql_error(db));
                mysql_close(db);
                return nullptr;
            }
            return new shield_db_conn{db};
        },
        // disconnect
        [](shield_db_conn* c) {
            if (!c) return;
            if (c->db) mysql_close(c->db);
            delete c;
        },
        // ping
        [](shield_db_conn* c) -> int {
            if (!c || !c->db) return 0;
            return mysql_ping(c->db) == 0 ? 1 : 0;
        },
        // query
        [](shield_db_conn* c, const char* sql, const char* const* params,
           int n_params, shield_db_result* out) -> int {
            if (!c || !c->db || !sql) {
                out->success = 0;
                out->error_msg = dup_string("mysql: invalid arguments");
                out->error_code = dup_string("db_query_failed");
                return 1;
            }
            return run_stmt(c->db, sql, params, n_params, true, out);
        },
        // execute
        [](shield_db_conn* c, const char* sql, const char* const* params,
           int n_params, shield_db_result* out) -> int {
            if (!c || !c->db || !sql) {
                out->success = 0;
                out->error_msg = dup_string("mysql: invalid arguments");
                out->error_code = dup_string("db_query_failed");
                return 1;
            }
            return run_stmt(c->db, sql, params, n_params, false, out);
        },
        // begin
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->db) return 1;
            return run_simple(c->db, "START TRANSACTION", out);
        },
        // commit
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->db) return 1;
            return run_simple(c->db, "COMMIT", out);
        },
        // rollback
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->db) return 1;
            return run_simple(c->db, "ROLLBACK", out);
        },
        // free_result
        [](shield_db_result* r) { clear_result(r); },
    };
    return v;
}
