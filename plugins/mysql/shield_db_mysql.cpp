// [SHIELD_DB_PLUGIN_MYSQL] MySQL backend plugin for shield_data.
//
// Implements the C ABI from shield/plugin/db_plugin.h on top of the
// MySQL Connector/C++ X DevAPI. All exceptions raised by mysqlx are
// caught at the boundary so they never cross the DLL edge; we surface
// them as soft failures via shield_db_result.error_msg / error_code.
//
// Error code mapping follows the conventions used elsewhere in shield:
//   "connection_lost"      Lost connection / server gone
//   "connection_timeout"   query timed out
//   "syntax_error"         SQL parse / grammar error
//   "constraint_violation" Duplicate key / FK / CHECK
//   "transaction_aborted"  Deadlock
//   "db_query_failed"      Catch-all for other mysqlx::Error

#include "shield/data/db_plugin.h"
#include "shield/plugin/plugin.h"

#include <mysqlx/xdevapi.h>

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

// Translate a mysqlx error message into a stable shield error code.
const char* map_mysqlx_error(const char* msg) {
    if (!msg) return "db_query_failed";
    std::string m(msg);
    if (m.find("Lost connection") != std::string::npos ||
        m.find("server has gone away") != std::string::npos)
        return "connection_lost";
    if (m.find("timeout") != std::string::npos ||
        m.find("timed out") != std::string::npos)
        return "connection_timeout";
    if (m.find("syntax") != std::string::npos ||
        m.find("SQL syntax") != std::string::npos)
        return "syntax_error";
    if (m.find("Duplicate") != std::string::npos ||
        m.find("foreign key") != std::string::npos ||
        m.find("constraint") != std::string::npos)
        return "constraint_violation";
    if (m.find("Deadlock") != std::string::npos)
        return "transaction_aborted";
    return "db_query_failed";
}

// Fill `out` from a mysqlx exception. Always returns 0 (soft failure).
int fill_exception(std::exception_ptr ep, shield_db_result* out) {
    out->success = 0;
    std::string msg;
    std::string code = "db_query_failed";
    try {
        std::rethrow_exception(ep);
    } catch (const mysqlx::Error& e) {
        msg = e.what();
        code = map_mysqlx_error(msg.c_str());
    } catch (const std::exception& e) {
        msg = e.what();
    } catch (...) {
        msg = "unknown plugin error";
    }
    out->error_msg = dup_string(msg.c_str());
    out->error_code = dup_string(code.c_str());
    return 0;
}

// Execute a SQL statement via a session. Selects whether to materialise
// rows (read path) or just affected counts (write path) via the
// `collect_rows` flag.
int run_stmt(mysqlx::Session& session, const char* sql,
             const char* const* params, int n_params,
             bool collect_rows, shield_db_result* out) {
    try {
        auto stmt = session.sql(sql);
        for (int i = 0; i < n_params; ++i) {
            stmt.bind(params ? params[i] : "");
        }
        auto result = stmt.execute();

        if (collect_rows) {
            std::vector<const char*> cells;
            int col_count = 0;
            int row_count = 0;
            for (auto row : result) {
                col_count = static_cast<int>(row.colCount());
                for (int c = 0; c < col_count; ++c) {
                    auto v = row[c];
                    if (v.isNull()) {
                        cells.push_back(nullptr);
                    } else {
                        std::string s = v.get<std::string>();
                        cells.push_back(dup_string(s.c_str()));
                    }
                }
                ++row_count;
            }
            out->success = 1;
            out->row_count = row_count;
            out->col_count = col_count;
            out->affected_rows = 0;
            out->last_insert_id = 0;
            if (row_count > 0) {
                out->cells = static_cast<const char**>(
                    std::malloc(sizeof(char*) * cells.size()));
                if (out->cells) {
                    std::memcpy(const_cast<char**>(out->cells),
                                cells.data(), sizeof(char*) * cells.size());
                }
            }
        } else {
            out->success = 1;
            out->affected_rows =
                static_cast<int64_t>(result.getAffectedItemsCount());
            out->last_insert_id =
                static_cast<int64_t>(result.getAutoIncrementValue());
            out->row_count = 0;
            out->col_count = 0;
            out->cells = nullptr;
        }
        return 0;
    } catch (...) {
        return fill_exception(std::current_exception(), out);
    }
}

int run_simple(mysqlx::Session& session, const char* sql,
               shield_db_result* out) {
    return run_stmt(session, sql, nullptr, 0, false, out);
}

}  // namespace

struct shield_db_conn {
    std::unique_ptr<mysqlx::Session> session;
};

extern "C" {

SHIELD_DB_EXPORT
const shield_db_plugin* shield_db_plugin_api(void) {
    static const shield_db_plugin plugin = {
        SHIELD_DB_ABI_VERSION,
        "mysql",
        "1.0.0",
        // connect
        [](const shield_db_connect_args* args,
           char* err_buf, int err_buf_size) -> shield_db_conn* {
            if (!args) return nullptr;
            try {
                auto s = std::make_unique<mysqlx::Session>(
                    args->host ? args->host : "localhost",
                    args->port ? args->port : 3306,
                    args->user ? args->user : "root",
                    args->password ? args->password : "",
                    args->database ? args->database : "shield");
                return new shield_db_conn{std::move(s)};
            } catch (const mysqlx::Error& e) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                }
                return nullptr;
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                }
                return nullptr;
            }
        },
        // disconnect
        [](shield_db_conn* c) {
            if (!c) return;
            if (c->session) {
                try { c->session->close(); } catch (...) {}
            }
            delete c;
        },
        // ping
        [](shield_db_conn* c) -> int {
            if (!c || !c->session) return 0;
            try {
                c->session->sql("SELECT 1").execute();
                return 1;
            } catch (...) {
                return 0;
            }
        },
        // query
        [](shield_db_conn* c, const char* sql,
           const char* const* params, int n_params,
           shield_db_result* out) -> int {
            if (!c || !c->session || !sql) {
                out->success = 0;
                out->error_msg = dup_string("mysql: invalid arguments");
                out->error_code = dup_string("db_query_failed");
                return 1;
            }
            return run_stmt(*c->session, sql, params, n_params, true, out);
        },
        // execute
        [](shield_db_conn* c, const char* sql,
           const char* const* params, int n_params,
           shield_db_result* out) -> int {
            if (!c || !c->session || !sql) {
                out->success = 0;
                out->error_msg = dup_string("mysql: invalid arguments");
                out->error_code = dup_string("db_query_failed");
                return 1;
            }
            return run_stmt(*c->session, sql, params, n_params, false, out);
        },
        // begin
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->session) return 1;
            return run_simple(*c->session, "START TRANSACTION", out);
        },
        // commit
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->session) return 1;
            return run_simple(*c->session, "COMMIT", out);
        },
        // rollback
        [](shield_db_conn* c, shield_db_result* out) -> int {
            if (!c || !c->session) return 1;
            return run_simple(*c->session, "ROLLBACK", out);
        },
        // free_result
        [](shield_db_result* r) { clear_result(r); },
    };
    return &plugin;
}

}  // extern "C"

// Generic plugin entry point — wraps the DB plugin for PluginManager.
namespace {
const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_DATABASE,
    "shield_db_mysql",
    "1.0.0",
    "MySQL X DevAPI database plugin",
    "Shield",
    nullptr, nullptr, nullptr, nullptr,
    nullptr,  // vtable set at runtime
};
}

extern "C" SHIELD_DB_EXPORT
const struct shield_plugin* shield_plugin_api(void) {
    // Lazily set vtable to point to the DB plugin.
    const_cast<shield_plugin&>(g_plugin).vtable = shield_db_plugin_api();
    return &g_plugin;
}
