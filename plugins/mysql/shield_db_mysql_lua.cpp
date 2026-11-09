// [SHIELD_PLUGIN] database.mysql — Lua facade: json param binding,
// result row mapping, statement/tx entry points and the
// shield.database.mysql namespace installer.
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

namespace {
// Typed parameter buffers for a JSON-bound statement (the worker twin of
// make_lua_params). MYSQL_BIND entries reference these, so the struct must
// outlive mysql_stmt_execute.
struct json_params {
    std::vector<MYSQL_BIND> binds;
    std::vector<int64_t> ints;
    std::vector<double> dbls;
    std::vector<std::string> strs;
    std::vector<unsigned long> lengths;
    std::vector<my_bool> nulls;
};

// Bind positional JSON values to `?` placeholders, mirroring make_lua_params'
// type mapping: null -> SQL NULL, bool -> 0/1, integers and floats bind
// natively, strings bind as text.
json_params make_json_params(const nlohmann::json& params) {
    json_params p;
    size_t n = params.is_array() ? params.size() : 0;
    p.binds.resize(n);
    p.ints.resize(n, 0);
    p.dbls.resize(n, 0.0);
    p.strs.resize(n);
    p.lengths.resize(n, 0);
    p.nulls.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const nlohmann::json& v = params[i];
        MYSQL_BIND& b = p.binds[i];
        b.is_null = &p.nulls[i];
        if (v.is_null()) {
            p.nulls[i] = 1;
            b.buffer_type = MYSQL_TYPE_NULL;
        } else if (v.is_boolean()) {
            p.ints[i] = v.get<bool>() ? 1 : 0;
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer = &p.ints[i];
            b.buffer_length = sizeof(int64_t);
        } else if (v.is_number_integer()) {
            p.ints[i] = v.get<int64_t>();
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer = &p.ints[i];
            b.buffer_length = sizeof(int64_t);
        } else if (v.is_number_float()) {
            p.dbls[i] = v.get<double>();
            b.buffer_type = MYSQL_TYPE_DOUBLE;
            b.buffer = &p.dbls[i];
            b.buffer_length = sizeof(double);
        } else {
            p.strs[i] = v.get<std::string>();
            p.lengths[i] = static_cast<unsigned long>(p.strs[i].size());
            b.buffer_type = MYSQL_TYPE_STRING;
            b.buffer = p.strs[i].data();
            b.buffer_length = p.lengths[i];
            b.length = &p.lengths[i];
        }
    }
    return p;
}
}  // namespace

// JSON twin of row_to_lua: column-name keyed object with the cell's wire
// typing preserved (int/double/string/null — never stringified numbers).
nlohmann::json row_to_json(const stmt_result& res, int r) {
    nlohmann::json row = nlohmann::json::object();
    for (int c = 0; c < res.col_count; ++c) {
        const std::string& name = res.col_names[c];
        switch (res.kinds[r][c]) {
            case kCellNull:
                row[name] = nullptr;
                break;
            case kCellInt:
                row[name] = res.ints[r][c];
                break;
            case kCellDbl:
                row[name] = res.dbls[r][c];
                break;
            default:
                row[name] = res.strs[r][c];
                break;
        }
    }
    return row;
}

// Worker-thread statement runner: the JSON twin of run_statement — same three
// modes, same result shapes (query -> array of row objects, query_one -> row
// object or null, execute -> {affected, last_insert_id}).
bool run_statement_json(MYSQL* mysql, const std::string& sql,
                        const nlohmann::json& params, const std::string& mode,
                        nlohmann::json* out, std::string* err_code,
                        std::string* err_msg) {
    stmt_error err;
    stmt_result res;
    json_params jp = make_json_params(params);
    if (!exec_typed(mysql, sql.c_str(), jp.binds, &res, &err)) {
        *err_code = err.code;
        *err_msg = err.msg;
        return false;
    }

    if (mode == "execute") {
        *out = nlohmann::json::object(
            {{"affected", res.affected}, {"last_insert_id", res.insert_id}});
        return true;
    }
    if (mode == "query_one") {
        // if/else, not a ternary: mixed json/nullptr arms fail to compile.
        if (!res.has_rows || res.row_count == 0) {
            *out = nullptr;  // no rows
        } else {
            *out = row_to_json(res, 0);
        }
        return true;
    }
    // "query" — sequence of rows.
    nlohmann::json rows = nlohmann::json::array();
    if (res.has_rows) {
        for (int r = 0; r < res.row_count; ++r) {
            rows.push_back(row_to_json(res, r));
        }
    }
    *out = std::move(rows);
    return true;
}

namespace {
// ---------------------------------------------------------------------------
// Lua helpers
//
// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.database.* facade.
// ---------------------------------------------------------------------------
shd::table make_error_table(shd::state_view lua, const char* code,
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
lua_params make_lua_params(const std::vector<shd::object>& values) {
    lua_params p;
    size_t n = values.size();
    p.binds.resize(n);
    p.ints.resize(n, 0);
    p.dbls.resize(n, 0.0);
    p.strs.resize(n);
    p.lengths.resize(n, 0);
    p.nulls.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const shd::object& v = values[i];
        MYSQL_BIND& b = p.binds[i];
        b.is_null = &p.nulls[i];
        if (!v.valid() || v == shd::nil) {
            p.nulls[i] = 1;
            b.buffer_type = MYSQL_TYPE_NULL;
            continue;
        }
        if (v.is<bool>()) {
            // Integer first — zero-arg is<number> probes would otherwise coerce
            // to double.
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
shd::table row_to_lua(shd::state_view lua, const stmt_result& res, int r) {
    auto t = lua.create_table();
    for (int c = 0; c < res.col_count; ++c) {
        const std::string& name = res.col_names[c];
        switch (res.kinds[r][c]) {
            case kCellNull:
                t[name] = shd::nil;
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
// shd::table iteration order is unspecified, so we bucket by numeric key
// first and then sort by index before binding.
std::vector<shd::object> collect_positional(
    const std::optional<shd::table>& params) {
    std::vector<shd::object> out;
    if (!params || !params->valid()) return out;
    // Gather (index, value) pairs where the key is a positive integer.
    std::map<lua_Integer, shd::object> bucket;
    for (const auto& kv : *params) {
        auto k = kv.first;
        if (k.get_type() != shd::type::number) continue;
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
shd::object run_statement(shd::state_view lua, MYSQL* mysql,
                          const std::string& sql,
                          const std::optional<shd::table>& params,
                          const char* mode,  // "query"|"query_one"|"execute"
                          bool* ok, shd::table* err_out, bool* broken) {
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
        return shd::nil;
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
        if (std::strcmp(mode, "query_one") == 0) return shd::nil;
        return shd::make_object(lua, lua.create_table());
    }

    if (std::strcmp(mode, "query_one") == 0) {
        *ok = true;
        if (res.row_count == 0) return shd::nil;
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
shd::table make_handle_proxy(shd::state_view lua, MYSQL* sess,
                             mysql_instance* inst);

// Build the per-instance proxy. Each top-level method acquires a connection
// from the pool (pool_guard returns it on scope exit).
shd::table make_instance_proxy(shd::state_view lua, mysql_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "__sync_query",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed",
                    acquire_err.empty() ? std::string("pool acquire failed")
                                        : acquire_err));
                return results;
            }
            bool ok = false;
            bool broken = false;
            shd::table err;
            shd::object rows = run_statement(lua, guard->sess, sql, params,
                                             "query", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? rows : shd::object(err));
            return results;
        });

    proxy.set_function(
        "__sync_query_one",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed",
                    acquire_err.empty() ? std::string("pool acquire failed")
                                        : acquire_err));
                return results;
            }
            bool ok = false;
            bool broken = false;
            shd::table err;
            shd::object row = run_statement(lua, guard->sess, sql, params,
                                            "query_one", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? row : shd::object(err));
            return results;
        });

    proxy.set_function(
        "__sync_execute",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed",
                    acquire_err.empty() ? std::string("pool acquire failed")
                                        : acquire_err));
                return results;
            }
            bool ok = false;
            bool broken = false;
            shd::table err;
            shd::object res = run_statement(lua, guard->sess, sql, params,
                                            "execute", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? res : shd::object(err));
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
            std::string acquire_err;
            auto guard = acquire_session(inst, &acquire_err);
            if (!guard || !*guard) {
                results.push_back(shd::make_object(lua, false));
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
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(lua, code, msg));
                    return results;
                }
            }

            // tx proxy shares this connection.
            shd::table tx = make_handle_proxy(lua, guard->sess, inst);
            shd::protected_function_result cb_res = callback(tx);
            bool commit = cb_res.valid();
            bool user_abort = false;

            if (cb_res.valid()) {
                shd::object first = cb_res.get(0);
                if (first.is<bool>() && !first.as<bool>()) {
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
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(lua, code, msg));
                    return results;
                }
            }

            if (!cb_res.valid()) {
                // Lua callback threw — we already rolled back.
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua,
                                                   "transaction_rolled_back",
                                                   "callback raised an error"));
                return results;
            }

            if (user_abort) {
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

    // Async entry (docs/db-async-design.md M3/M4): register the suspension
    // via the host primitive, then hand the task to the worker pool. Suspend
    // happens BEFORE enqueue so a fast worker completion always finds its
    // session (the host's yield-window guard handles the requeue). Returns
    // the session id, or 0 when the caller must run synchronously. The
    // method string is the shim's vocabulary: the three run modes plus the
    // __tx_* transaction protocol forms.
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
            mysql_task task;
            task.kind = task_kind::stmt;
            const std::string tag = "db:mysql:" + method;
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
                        task.params.push_back(v.as<lua_Integer>());
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
            if (session == 0) return 0;
            task.session = session;
            submit_async(inst, std::move(task));
            return session;
        });

    // Wrap the sync methods with the shared submit-and-yield shim. If the
    // shim source fails to load (should be impossible — it is a string
    // constant), fall back to exposing the sync methods directly.
    shd::load_result shim =
        lua.load(shield::plugins::kDbAsyncShimLua, "=db_async_shim");
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
            inst->host_api->log(SHIELD_LOG_WARN, "database.mysql",
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

// Proxy whose methods reuse the transaction's connection (used inside
// transactions). The MYSQL* is owned by transaction()'s pool_guard, which
// outlives the tx table.
shd::table make_handle_proxy(shd::state_view lua, MYSQL* sess,
                             mysql_instance* /*inst*/) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "query",
        [sess](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            bool broken = false;
            shd::table err;
            shd::object rows = run_statement(lua, sess, sql, params, "query",
                                             &ok, &err, &broken);
            (void)broken;  // tx connection lifecycle is owned by transaction()
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? rows : shd::object(err));
            return results;
        });

    proxy.set_function(
        "query_one",
        [sess](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            bool broken = false;
            shd::table err;
            shd::object row = run_statement(lua, sess, sql, params, "query_one",
                                            &ok, &err, &broken);
            (void)broken;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? row : shd::object(err));
            return results;
        });

    proxy.set_function(
        "execute",
        [sess](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            bool broken = false;
            shd::table err;
            shd::object res = run_statement(lua, sess, sql, params, "execute",
                                            &ok, &err, &broken);
            (void)broken;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? res : shd::object(err));
            return results;
        });

    return proxy;
}
}  // namespace

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
    shd::state_view lua(L);

    // Build the callable namespace shield.database.mysql.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table database =
        shield::plugins::get_or_create_subtable(shield, "database");

    shd::object existing = database["mysql"];
    if (!existing.is<shd::table>()) {
        auto* owner = reinterpret_cast<mysql_instance*>(self);
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
                shield::plugins::apply_db_mapper_api(lua, proxy);
                results.push_back(shd::make_object(lua, proxy));
                return results;
            });
        shield::plugins::set_metatable(ns, mt);
        database["mysql"] = ns;
    }

    return 0;
}
