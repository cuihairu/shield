// [SHIELD_PLUGIN] database.mysql — MySQL provider for shield.database.v1.
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
// connection to the pool on scope exit. The C vtable still creates/closes a
// fresh connection per connect — C-ABI callers do NOT get pooling, only Lua
// callers do. This keeps the vtable semantics unchanged while giving Lua
// scripts the warm-connection performance they expect.

#include <mysql/errmsg.h>
#include <mysql/mysql.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <sol/sol.hpp>
#include <string>
#include <vector>

#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"
#include "shield/plugin/host_api.h"
#include "shield_db_mapper.hpp"
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

// ---------------------------------------------------------------------------
// Prepared statement engine
//
// Statements run through mysql_stmt_* so `?` placeholders are bound, never
// spliced. Result rows are fetched with per-column typed buffers (chosen from
// the column's wire type) so Lua callers keep their type semantics — an INT
// column comes back as a Lua integer, not "42".
// ---------------------------------------------------------------------------

// Cell kinds for stmt_result::kinds.
enum : unsigned char {
    kCellInt = 'i',
    kCellDbl = 'd',
    kCellStr = 's',
    kCellNull = 'n'
};

struct stmt_result {
    bool has_rows = false;
    int col_count = 0;
    int row_count = 0;
    std::vector<std::string> col_names;
    // Per [row][col] typed cells; exactly one of the three is live per cell,
    // selected by kinds[row][col].
    std::vector<std::vector<int64_t>> ints;
    std::vector<std::vector<double>> dbls;
    std::vector<std::vector<std::string>> strs;
    std::vector<std::vector<unsigned char>> kinds;
    int64_t affected = 0;
    int64_t insert_id = 0;
};

struct stmt_error {
    std::string code;
    std::string msg;
};

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

}  // namespace

// ---------------------------------------------------------------------------
// v1 ABI entry. The instance carries its own config (parsed from
// config_json) and a per-instance Session pool. It registers itself in a
// process-wide map so the Lua callable namespace can resolve plugins.bindings
// logical names to instances. The C++ vtable is also served through
// get_interface() for any C-ABI consumer that prefers the raw connect/query
// surface.
// ---------------------------------------------------------------------------
namespace {

struct mysql_instance {
    shield_plugin_instance_v1 shell;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    std::string instance_id;

    // Parsed config (from args->config_json). Defaults match manifest.yaml.
    std::string host = "127.0.0.1";
    int port = 3306;  // classic MySQL client/server protocol port
    std::string database;
    std::string username;
    std::string password;
    int connect_timeout_ms = 5000;
    int query_timeout_ms = 5000;
    int pool_size = 4;
    int acquire_timeout_ms = 10000;  // how long to wait when the pool is full

    // Pool state — protected by pool_mu.
    std::mutex pool_mu;
    std::condition_variable pool_cv;
    std::queue<MYSQL*> free_list;
    int current_size = 0;  // live connections (in free_list + checked out)
};

// Process-wide registry: instance_id -> mysql_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Read on every proxy creation, so it must be
// thread-safe.
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, mysql_instance*>& instances_map() {
    static std::map<std::string, mysql_instance*> m;
    return m;
}
void register_instance(mysql_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
mysql_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract the known keys and fall
// back to defaults for anything missing.
void parse_instance_config(mysql_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string())
            inst->host = j["host"].get<std::string>();
        if (j.contains("port") && j["port"].is_number_integer())
            inst->port = j["port"].get<int>();
        if (j.contains("database") && j["database"].is_string())
            inst->database = j["database"].get<std::string>();
        if (j.contains("username") && j["username"].is_string())
            inst->username = j["username"].get<std::string>();
        if (j.contains("user") && j["user"].is_string())
            inst->username = j["user"].get<std::string>();  // alias
        if (j.contains("password") && j["password"].is_string())
            inst->password = j["password"].get<std::string>();
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer())
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        if (j.contains("query_timeout_ms") &&
            j["query_timeout_ms"].is_number_integer())
            inst->query_timeout_ms = j["query_timeout_ms"].get<int>();
        if (j.contains("pool_size") && j["pool_size"].is_number_integer())
            inst->pool_size = j["pool_size"].get<int>();
        if (j.contains("acquire_timeout_ms") &&
            j["acquire_timeout_ms"].is_number_integer())
            inst->acquire_timeout_ms = j["acquire_timeout_ms"].get<int>();
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

// ---------------------------------------------------------------------------
// Connection pool
//
// A TCP + auth handshake is expensive, so the Lua proxy keeps a per-instance
// free list of live MYSQL* connections. acquire_session() returns a RAII
// guard that pushes the connection back onto the free list (and notifies one
// waiter) when it goes out of scope.
//
// Lifecycle:
//   - Try free_list first (fast path, no construction).
//   - If free_list is empty and current_size < pool_size, open a new
//     connection (current_size is bumped under the lock so concurrent
//     openers don't overshoot).
//   - Otherwise wait on pool_cv up to acquire_timeout_ms, then fail.
//
// Broken connections are discarded (not pushed back) so the next acquirer
// gets a fresh one. current_size is decremented when a connection is dropped.
// ---------------------------------------------------------------------------

// Open a new connection from the instance config. Returns nullptr on failure
// and fills *err with the server's message.
MYSQL* open_conn(const mysql_instance* inst, std::string* err) {
    MYSQL* db = mysql_init(nullptr);
    if (!db) {
        if (err) *err = "mysql: out of memory";
        return nullptr;
    }
    unsigned int connect_timeout = static_cast<unsigned int>(
        inst->connect_timeout_ms > 0 ? (inst->connect_timeout_ms + 999) / 1000
                                     : 5);
    // Approximate the per-statement query timeout with socket read/write
    // timeouts (the client library has no per-statement deadline).
    unsigned int query_timeout = static_cast<unsigned int>(
        inst->query_timeout_ms > 0 ? (inst->query_timeout_ms + 999) / 1000 : 5);
    mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &query_timeout);
    mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &query_timeout);
    mysql_options(db, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(
            db, inst->host.c_str(),
            inst->username.empty() ? "root" : inst->username.c_str(),
            inst->password.c_str(), inst->database.c_str(),
            inst->port > 0 ? static_cast<unsigned int>(inst->port) : 3306,
            nullptr, 0)) {
        if (err) *err = std::string("mysql connect: ") + mysql_error(db);
        mysql_close(db);
        return nullptr;
    }
    return db;
}

// RAII guard — releases the connection back to the pool on destruction. If
// the connection is marked broken (e.g. the caller observed a connection
// error), the guard drops it instead of returning a known-bad connection.
struct pool_guard {
    mysql_instance* inst = nullptr;
    MYSQL* sess = nullptr;
    bool broken = false;

    pool_guard() = default;
    pool_guard(mysql_instance* i, MYSQL* s) : inst(i), sess(s) {}

    ~pool_guard() {
        if (!inst || !sess) return;
        if (broken) {
            // Discard: close (best effort), then decrement live count.
            mysql_close(sess);
            std::lock_guard lk(inst->pool_mu);
            inst->current_size -= 1;
            inst->pool_cv.notify_one();
            return;
        }
        std::lock_guard lk(inst->pool_mu);
        inst->free_list.push(sess);
        inst->pool_cv.notify_one();
    }

    pool_guard(const pool_guard&) = delete;
    pool_guard& operator=(const pool_guard&) = delete;
    pool_guard(pool_guard&& o) noexcept
        : inst(o.inst), sess(o.sess), broken(o.broken) {
        o.inst = nullptr;
        o.sess = nullptr;
    }
    pool_guard& operator=(pool_guard&& o) noexcept {
        if (this != &o) {
            inst = o.inst;
            sess = o.sess;
            broken = o.broken;
            o.inst = nullptr;
            o.sess = nullptr;
        }
        return *this;
    }

    MYSQL* operator->() const { return sess; }
    MYSQL& operator*() const { return *sess; }
    explicit operator bool() const { return sess != nullptr; }
};

// Acquire a session from the pool. On success returns a guard whose `sess`
// is non-null. On failure returns a guard with sess == nullptr and fills
// *err (if non-null).
//
// Three outcomes under the lock:
//   - free_list non-empty          -> pop and return (fast path)
//   - current_size < pool_size     -> reserve a slot (current_size++) and open
//                                     a fresh Session OUTSIDE the lock so a
//                                     slow connect doesn't stall other waiters
//   - pool full                    -> wait on pool_cv up to acquire_timeout_ms
//
// Broken sessions (set by run_statement on connection-lost errors) are
// discarded by the guard's destructor and current_size is decremented so the
// next acquirer can open a replacement.
std::unique_ptr<pool_guard> acquire_session(mysql_instance* inst,
                                            std::string* err) {
    if (!inst) {
        if (err) *err = "mysql: null instance";
        return std::make_unique<pool_guard>();
    }

    // Outcome enum for the in-lock probe: TAKE (free connection), OPEN (slot
    // reserved, must construct outside the lock), WAIT (pool full).
    enum class Probe { take, open, wait };
    MYSQL* taken = nullptr;
    Probe probe;
    {
        std::lock_guard lk(inst->pool_mu);
        if (!inst->free_list.empty()) {
            taken = inst->free_list.front();
            inst->free_list.pop();
            probe = Probe::take;
        } else if (inst->current_size < inst->pool_size) {
            inst->current_size += 1;  // reserve the slot
            probe = Probe::open;
        } else {
            probe = Probe::wait;
        }
    }

    if (probe == Probe::take) {
        return std::make_unique<pool_guard>(inst, taken);
    }

    if (probe == Probe::open) {
        std::string open_err;
        MYSQL* db = open_conn(inst, &open_err);
        if (db) {
            return std::make_unique<pool_guard>(inst, db);
        }
        if (err) *err = open_err;
        // Open failed — release the slot and wake one waiter.
        std::lock_guard lk(inst->pool_mu);
        inst->current_size -= 1;
        inst->pool_cv.notify_one();
        return std::make_unique<pool_guard>();
    }

    // probe == Probe::wait
    std::unique_lock lk(inst->pool_mu);
    bool got = inst->pool_cv.wait_for(
        lk,
        std::chrono::milliseconds(
            inst->acquire_timeout_ms > 0 ? inst->acquire_timeout_ms : 10000),
        [&] {
            return !inst->free_list.empty() ||
                   inst->current_size < inst->pool_size;
        });
    if (!got) {
        if (err) *err = "mysql: connection pool exhausted (acquire timeout)";
        return std::make_unique<pool_guard>();
    }
    if (!inst->free_list.empty()) {
        MYSQL* s = inst->free_list.front();
        inst->free_list.pop();
        lk.unlock();
        return std::make_unique<pool_guard>(inst, s);
    }
    // Slot opened up — reserve and open outside the lock.
    inst->current_size += 1;
    lk.unlock();
    std::string open_err;
    MYSQL* db = open_conn(inst, &open_err);
    if (db) {
        return std::make_unique<pool_guard>(inst, db);
    }
    if (err) *err = open_err;
    std::lock_guard lk2(inst->pool_mu);
    inst->current_size -= 1;
    inst->pool_cv.notify_one();
    return std::make_unique<pool_guard>();
}

// Drain the pool on shutdown. Assumes no other thread is acquiring (host
// guarantees shutdown is the last call on the instance).
void drain_pool(mysql_instance* inst) {
    std::lock_guard lk(inst->pool_mu);
    while (!inst->free_list.empty()) {
        MYSQL* s = inst->free_list.front();
        inst->free_list.pop();
        mysql_close(s);
    }
    inst->current_size = 0;
}

// ---------------------------------------------------------------------------
// Lua helpers
//
// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.database.* facade.
// ---------------------------------------------------------------------------
sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Typed parameter buffers for a Lua-bound statement. MYSQL_BIND entries
// reference these, so the struct must outlive mysql_stmt_execute.
struct lua_params {
    std::vector<MYSQL_BIND> binds;
    std::vector<int64_t> ints;
    std::vector<double> dbls;
    std::vector<std::string> strs;
    std::vector<unsigned long> lengths;
    std::vector<my_bool> nulls;
};

// Bind positional Lua values to `?` placeholders. nil -> SQL NULL;
// bool -> 0/1; integer and number bind natively; strings bind as text;
// anything else falls back to its stringified form (mirroring the sqlite
// plugin's tolerant bind behaviour).
lua_params make_lua_params(const std::vector<sol::object>& values) {
    lua_params p;
    size_t n = values.size();
    p.binds.resize(n);
    p.ints.resize(n, 0);
    p.dbls.resize(n, 0.0);
    p.strs.resize(n);
    p.lengths.resize(n, 0);
    p.nulls.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const sol::object& v = values[i];
        MYSQL_BIND& b = p.binds[i];
        b.is_null = &p.nulls[i];
        if (!v.valid() || v == sol::lua_nil) {
            p.nulls[i] = 1;
            b.buffer_type = MYSQL_TYPE_NULL;
            continue;
        }
        if (v.is<bool>()) {
            // Integer first — sol2 would otherwise coerce to double.
            p.ints[i] = v.as<bool>() ? 1 : 0;
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer = &p.ints[i];
            b.buffer_length = sizeof(int64_t);
            continue;
        }
        if (v.is<lua_Integer>()) {
            p.ints[i] = static_cast<int64_t>(v.as<lua_Integer>());
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer = &p.ints[i];
            b.buffer_length = sizeof(int64_t);
            continue;
        }
        if (v.is<double>()) {
            p.dbls[i] = v.as<double>();
            b.buffer_type = MYSQL_TYPE_DOUBLE;
            b.buffer = &p.dbls[i];
            b.buffer_length = sizeof(double);
            continue;
        }
        std::string s;
        if (v.is<std::string>()) {
            s = v.as<std::string>();
        } else {
            try {
                s = v.as<std::string>();  // stringify fallback
            } catch (...) {
                p.nulls[i] = 1;
                b.buffer_type = MYSQL_TYPE_NULL;
                continue;
            }
        }
        p.strs[i] = std::move(s);
        p.lengths[i] = static_cast<unsigned long>(p.strs[i].size());
        b.buffer_type = MYSQL_TYPE_STRING;
        b.buffer = p.strs[i].data();
        b.buffer_length = p.lengths[i];
        b.length = &p.lengths[i];
    }
    return p;
}

// Convert result row `r` into a Lua table keyed by column name.
sol::table row_to_lua(sol::state_view lua, const stmt_result& res, int r) {
    auto t = lua.create_table();
    for (int c = 0; c < res.col_count; ++c) {
        const std::string& name = res.col_names[c];
        switch (res.kinds[r][c]) {
            case kCellNull:
                t[name] = sol::lua_nil;
                break;
            case kCellInt:
                t[name] = static_cast<lua_Integer>(res.ints[r][c]);
                break;
            case kCellDbl:
                t[name] = static_cast<lua_Number>(res.dbls[r][c]);
                break;
            default:
                t[name] = res.strs[r][c];
                break;
        }
    }
    return t;
}

// Collect positional params from a Lua table (sequence keys 1..N) in order.
// sol::table iteration order is unspecified, so we bucket by numeric key
// first and then sort by index before binding.
std::vector<sol::object> collect_positional(
    const sol::optional<sol::table>& params) {
    std::vector<sol::object> out;
    if (!params || !params->valid()) return out;
    // Gather (index, value) pairs where the key is a positive integer.
    std::map<lua_Integer, sol::object> bucket;
    for (auto& kv : *params) {
        auto k = kv.first;
        if (k.get_type() != sol::type::number) continue;
        lua_Integer idx = k.as<lua_Integer>();
        if (idx >= 1) bucket[idx] = kv.second;
    }
    for (auto& [idx, val] : bucket) out.push_back(val);
    return out;
}

// Run a statement on `mysql` and return one of:
//   "query"     -> sequence table {row1, row2, ...}
//   "query_one" -> single row table or nil
//   "execute"   -> table {affected=N, last_insert_id=M}
//
// On error, *ok is set to false and *err_out receives an error table. The
// `broken` flag is set when the error looks like a connection-lost so the
// caller can drop the pooled connection.
sol::object run_statement(sol::state_view lua, MYSQL* mysql,
                          const std::string& sql,
                          const sol::optional<sol::table>& params,
                          const char* mode,  // "query"|"query_one"|"execute"
                          bool* ok, sol::table* err_out, bool* broken) {
    stmt_error err;
    stmt_result res;
    if (!exec_typed(mysql, sql.c_str(),
                    make_lua_params(collect_positional(params)).binds, &res,
                    &err)) {
        *ok = false;
        if (broken) {
            *broken = (err.code == "connection_lost" ||
                       err.code == "connection_timeout");
        }
        *err_out = make_error_table(lua, err.code.c_str(), err.msg);
        return sol::lua_nil;
    }
    if (broken) *broken = false;

    if (std::strcmp(mode, "execute") == 0) {
        auto t = lua.create_table();
        t["affected"] = static_cast<lua_Integer>(res.affected);
        t["last_insert_id"] = static_cast<lua_Integer>(res.insert_id);
        *ok = true;
        return t;
    }

    if (!res.has_rows) {
        // Query on a statement that produced no result set.
        *ok = true;
        if (std::strcmp(mode, "query_one") == 0) return sol::lua_nil;
        return sol::make_object(lua, lua.create_table());
    }

    if (std::strcmp(mode, "query_one") == 0) {
        *ok = true;
        if (res.row_count == 0) return sol::lua_nil;
        return row_to_lua(lua, res, 0);
    }

    // "query" — sequence of rows.
    auto rows = lua.create_table();
    int idx = 1;
    for (int r = 0; r < res.row_count; ++r) {
        rows[idx++] = row_to_lua(lua, res, r);
    }
    *ok = true;
    return rows;
}

// Forward decl — make_handle_proxy is used by transaction().
sol::table make_handle_proxy(sol::state_view lua, MYSQL* sess,
                             mysql_instance* inst);

// Execute a transaction-control statement (BEGIN/COMMIT/ROLLBACK) on a
// pooled connection. On failure fills *code/*msg with the mapped error.
bool run_tx_sql(MYSQL* mysql, const char* sql, const char** code,
                std::string* msg) {
    if (mysql_real_query(mysql, sql,
                         static_cast<unsigned long>(std::strlen(sql))) == 0) {
        return true;
    }
    *code = map_mysql_error(mysql_errno(mysql), mysql_error(mysql));
    *msg = std::string(sql) + ": " + mysql_error(mysql);
    return false;
}

// Build the per-instance proxy. Each top-level method acquires a connection
// from the pool (pool_guard returns it on scope exit).
sol::table make_instance_proxy(sol::state_view lua, mysql_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "query",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed",
                    acquire_err.empty() ? std::string("pool acquire failed")
                                        : acquire_err));
                return results;
            }
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object rows = run_statement(lua, guard->sess, sql, params,
                                             "query", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? rows : err);
            return results;
        });

    proxy.set_function(
        "query_one",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed",
                    acquire_err.empty() ? std::string("pool acquire failed")
                                        : acquire_err));
                return results;
            }
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object row = run_statement(lua, guard->sess, sql, params,
                                            "query_one", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? row : err);
            return results;
        });

    proxy.set_function(
        "execute",
        [inst](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed",
                    acquire_err.empty() ? std::string("pool acquire failed")
                                        : acquire_err));
                return results;
            }
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object res = run_statement(lua, guard->sess, sql, params,
                                            "execute", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? res : err);
            return results;
        });

    proxy.set_function(
        "transaction",
        [inst](sol::this_state s,
               sol::protected_function callback) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed",
                    acquire_err.empty() ? std::string("pool acquire failed")
                                        : acquire_err));
                return results;
            }

            // BEGIN
            {
                const char* code = nullptr;
                std::string msg;
                if (!run_tx_sql(guard->sess, "START TRANSACTION", &code,
                                &msg)) {
                    guard->broken = true;
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua, code, msg));
                    return results;
                }
            }

            // tx proxy shares this connection.
            sol::table tx = make_handle_proxy(lua, guard->sess, inst);
            sol::protected_function_result cb_res = callback(tx);
            bool commit = cb_res.valid();
            bool user_abort = false;

            if (cb_res.valid()) {
                sol::optional<bool> first = cb_res.get<sol::optional<bool>>(0);
                if (first && !*first) {
                    commit = false;
                    user_abort = true;
                }
            }

            const char* tx_sql = commit ? "COMMIT" : "ROLLBACK";
            {
                const char* code = nullptr;
                std::string msg;
                // COMMIT/ROLLBACK failure — typically connection lost or
                // deadlock during commit. Mark broken so the pool drops it.
                if (!run_tx_sql(guard->sess, tx_sql, &code, &msg)) {
                    guard->broken = true;
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua, code, msg));
                    return results;
                }
            }

            if (!cb_res.valid()) {
                // Lua callback threw — we already rolled back.
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                                                   "transaction_rolled_back",
                                                   "callback raised an error"));
                return results;
            }

            if (user_abort) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "transaction_rolled_back", "callback returned false"));
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

// Proxy whose methods reuse the transaction's connection (used inside
// transactions). The MYSQL* is owned by transaction()'s pool_guard, which
// outlives the tx table.
sol::table make_handle_proxy(sol::state_view lua, MYSQL* sess,
                             mysql_instance* /*inst*/) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "query",
        [sess](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object rows = run_statement(lua, sess, sql, params, "query",
                                             &ok, &err, &broken);
            (void)broken;  // tx connection lifecycle is owned by transaction()
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? rows : err);
            return results;
        });

    proxy.set_function(
        "query_one",
        [sess](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object row = run_statement(lua, sess, sql, params, "query_one",
                                            &ok, &err, &broken);
            (void)broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? row : err);
            return results;
        });

    proxy.set_function(
        "execute",
        [sess](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object res = run_statement(lua, sess, sql, params, "execute",
                                            &ok, &err, &broken);
            (void)broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? res : err);
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err) {
    // register_lua installs the shared, idempotent callable namespace
    // shield.database.mysql. Lua passes a binding logical name; PluginHost
    // resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.mysql: lua_State is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    // Build the callable namespace shield.database.mysql.
    auto shield = lua["shield"].get_or_create<sol::table>();
    auto database = shield["database"].get_or_create<sol::table>();

    sol::object existing = database["mysql"];
    if (!existing.is<sol::table>()) {
        auto* owner = reinterpret_cast<mysql_instance*>(self);
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        mt.set_function(
            "__call",
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
        database["mysql"] = ns;
    }

    return 0;
}

int mysql_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new mysql_instance;
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
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) {
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        // shell is the first member of mysql_instance (offset 0), so self
        // points at the enclosing mysql_instance. Standard C-ABI pattern.
        auto* inst = reinterpret_cast<mysql_instance*>(self);
        unregister_instance(inst->instance_id);
        drain_pool(inst);
        delete inst;
    };
    inst->shell.register_lua = &register_lua_impl;
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.mysql",
        "1.0.0",
        mysql_create,
    };
    return &abi;
}
