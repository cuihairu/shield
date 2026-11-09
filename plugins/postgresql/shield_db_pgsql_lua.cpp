// [SHIELD_PLUGIN] database.postgresql — Lua facade: error tables,
// row converters, the synchronous statement runner, the instance/
// handle proxies and the shield.database.postgresql namespace
// installer.
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

namespace {
// ---------------------------------------------------------------------------
// Lua helpers.
// ---------------------------------------------------------------------------

// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.database.* facade.
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Build a Lua error table from a PGresult (pulls SQLSTATE + primary message).
shd::table make_error_from_result(shd::state_view lua, PGresult* res) {
    const char* sqlstate =
        res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : nullptr;
    const char* msg = res ? PQresultErrorMessage(res) : nullptr;
    return make_error_table(lua, map_sqlstate(sqlstate),
                            msg && msg[0]
                                ? std::string(msg)
                                : std::string("postgresql: query failed"));
}

// Convert the value of `res` at (row, col) into a Lua object. PostgreSQL
// returns everything as text via PQgetvalue; we rely on the field's OID
// (PQftype) only to distinguish "definitely integer" (INT2/INT4/INT8/OID)
// from "definitely float" (FLOAT4/FLOAT8/NUMERIC) from text. NULL stays nil.
shd::object pg_cell_to_lua(shd::state_view lua, PGresult* res, int row,
                           int col) {
    if (PQgetisnull(res, row, col)) return shd::nil;
    const char* v = PQgetvalue(res, row, col);
    if (!v) return shd::nil;
    Oid t = PQftype(res, col);
    std::string s = v;
    // Numeric types: INT2=21, INT4=23, INT8=20, OID=26. BOOL=16.
    if (t == 21 || t == 23 || t == 20 || t == 26) {
        try {
            return shd::make_object(
                lua, static_cast<lua_Integer>(std::strtoll(v, nullptr, 10)));
        } catch (...) { /* fall through */
        }
    }
    if (t == 16) {
        // boolean — libpq renders 't'/'f'.
        return shd::make_object(lua, s == "t");
    }
    if (t == 700 || t == 701 || t == 1700) {
        try {
            return shd::make_object(lua, std::stod(s));
        } catch (...) { /* fall through */
        }
    }
    return shd::make_object(lua, s);
}

// Build a Lua row table from a PGresult at row `r`, keyed by column name.
shd::table pg_row_to_lua(shd::state_view lua, PGresult* res, int r) {
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

lua_params collect_lua_params(std::optional<shd::table> params,
                              std::string* err_msg) {
    lua_params out;
    if (!params || !params->valid()) return out;
    // Iterate once, copying values so later binding can't invalidate them.
    std::vector<shd::object> positional;
    for (const auto& kv : *params) {
        if (kv.first.get_type() != shd::type::number) continue;
        positional.push_back(kv.second);
    }
    out.storage.reserve(positional.size());
    out.ptrs.reserve(positional.size());
    for (size_t i = 0; i < positional.size(); ++i) {
        const shd::object& v = positional[i];
        if (!v.valid() || v == shd::nil) {
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
            if (err_msg)
                *err_msg = "unsupported parameter type at index " +
                           std::to_string(i + 1) + "; bound as NULL";
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
shd::object run_statement(
    shd::state_view lua, PGconn* conn, const std::string& sql,
    std::optional<shd::table> params,
    const char* mode,  // "query" | "query_one" | "execute"
    bool* ok, shd::table* err_out, bool* conn_bad) {
    std::string bind_err;
    lua_params lp = collect_lua_params(params, &bind_err);

    std::string rewritten = rewrite_placeholders(sql.c_str());

    PGresult* res =
        PQexecParams(conn, rewritten.c_str(), static_cast<int>(lp.ptrs.size()),
                     nullptr, lp.ptrs.data(), nullptr, nullptr, 0);
    if (!res) {
        // Out of memory on libpq side — conn is almost certainly toast.
        *ok = false;
        *conn_bad = true;
        *err_out = make_error_table(lua, "connection_lost",
                                    "postgresql: PQexecParams returned NULL");
        return shd::nil;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        *err_out = make_error_from_result(lua, res);
        if (PQstatus(conn) == CONNECTION_BAD) *conn_bad = true;
        PQclear(res);
        *ok = false;
        return shd::nil;
    }

    shd::object result = shd::nil;
    if (std::strcmp(mode, "execute") == 0) {
        char* affected = PQcmdTuples(res);
        auto t = lua.create_table();
        t["affected"] =
            affected && affected[0]
                ? static_cast<lua_Integer>(std::strtoll(affected, nullptr, 10))
                : 0;
        result = t;
    } else if (std::strcmp(mode, "query_one") == 0) {
        if (PQntuples(res) > 0) {
            result = pg_row_to_lua(lua, res, 0);
        } else {
            result = shd::object(shd::nil);  // no rows
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
shd::table make_handle_proxy(shd::state_view lua, pgsql_instance* inst,
                             PGconn* conn, bool* conn_bad);

// Build a per-instance proxy table. Each method acquires from the pool,
// runs the statement, and returns the conn via RAII.
shd::table make_instance_proxy(shd::state_view lua, pgsql_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "__sync_query",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            bool ok = false, conn_bad = false;
            shd::table err_t;
            shd::object rows = run_statement(lua, g.get(), sql, params, "query",
                                             &ok, &err_t, &conn_bad);
            g.bad = conn_bad;  // let pool_guard PQfinish dead conns
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? rows : shd::object(err_t));
            return results;
        });

    proxy.set_function(
        "__sync_query_one",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            bool ok = false, conn_bad = false;
            shd::table err_t;
            shd::object row = run_statement(
                lua, g.get(), sql, params, "query_one", &ok, &err_t, &conn_bad);
            g.bad = conn_bad;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? row : shd::object(err_t));
            return results;
        });

    proxy.set_function(
        "__sync_execute",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            bool ok = false, conn_bad = false;
            shd::table err_t;
            shd::object res = run_statement(lua, g.get(), sql, params,
                                            "execute", &ok, &err_t, &conn_bad);
            g.bad = conn_bad;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? res : shd::object(err_t));
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
            std::string err;
            pool_guard g = acquire_conn(inst, &err);
            if (!g.get()) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
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
                        shd::table err_t = make_error_from_result(lua, res);
                        if (PQstatus(g.get()) == CONNECTION_BAD)
                            conn_bad = true;
                        PQclear(res);
                        g.bad = conn_bad;
                        results.push_back(shd::make_object(lua, false));
                        results.push_back(err_t);
                        return results;
                    }
                    PQclear(res);
                }
            }

            // Build the tx proxy sharing this conn.
            bool tx_conn_bad = false;
            shd::table tx = make_handle_proxy(lua, inst, g.get(), &tx_conn_bad);

            shd::protected_function_result cb_res = callback(tx);
            bool commit = cb_res.valid();
            bool user_abort = false;
            bool saw_error = false;

            if (cb_res.valid()) {
                // A boolean `false` first return is treated as user-initiated
                // rollback (matches the host facade's contract).
                shd::object first = cb_res.get(0);
                if (first.is<bool>() && !first.as<bool>()) {
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
                        const char* sqlstate =
                            PQresultErrorField(res, PG_DIAG_SQLSTATE);
                        tx_err_code = map_sqlstate(sqlstate);
                        const char* m = PQresultErrorMessage(res);
                        tx_err_msg =
                            m && m[0] ? m : (std::string(tx_sql) + " failed");
                        if (PQstatus(g.get()) == CONNECTION_BAD)
                            conn_bad = true;
                    }
                    PQclear(res);
                }
            }
            g.bad = conn_bad;

            if (!tx_ok) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, tx_err_code, tx_err_msg));
                return results;
            }

            if (saw_error) {
                // Callback raised — soft failure after successful rollback.
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
            pg_task task;
            task.kind = pg_task_kind::stmt;
            const std::string tag = "db:postgresql:" + method;
            if (method == "__tx_begin") {
                task.kind = pg_task_kind::tx_begin;
            } else if (method == "__tx_query" || method == "__tx_query_one" ||
                       method == "__tx_execute") {
                task.kind = pg_task_kind::tx_stmt;
                task.method = method.substr(5);  // "__tx_" -> run mode
            } else if (method == "__tx_commit") {
                task.kind = pg_task_kind::tx_commit;
            } else if (method == "__tx_rollback") {
                task.kind = pg_task_kind::tx_rollback;
            } else {
                task.method = std::move(method);
            }
            task.tx_token = tx_token;
            if (task.kind == pg_task_kind::stmt ||
                task.kind == pg_task_kind::tx_stmt) {
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
            inst->host_api->log(SHIELD_LOG_WARN, "database.postgresql",
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

// Proxy bound to a single checked-out connection (used inside transactions).
// Methods don't acquire from the pool — they run statements directly on
// `conn`. `*conn_bad` is shared with the transaction() lambda so the
// caller knows whether COMMIT is still safe.
shd::table make_handle_proxy(shd::state_view lua, pgsql_instance* /*inst*/,
                             PGconn* conn, bool* conn_bad) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "query",
        [conn, conn_bad](
            shd::this_state s, std::string sql,
            std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false, cd = false;
            shd::table err_t;
            shd::object rows = run_statement(lua, conn, sql, params, "query",
                                             &ok, &err_t, &cd);
            if (cd) *conn_bad = true;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? rows : shd::object(err_t));
            return results;
        });

    proxy.set_function(
        "query_one",
        [conn, conn_bad](
            shd::this_state s, std::string sql,
            std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false, cd = false;
            shd::table err_t;
            shd::object row = run_statement(lua, conn, sql, params, "query_one",
                                            &ok, &err_t, &cd);
            if (cd) *conn_bad = true;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? row : shd::object(err_t));
            return results;
        });

    proxy.set_function(
        "execute",
        [conn, conn_bad](
            shd::this_state s, std::string sql,
            std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false, cd = false;
            shd::table err_t;
            shd::object res = run_statement(lua, conn, sql, params, "execute",
                                            &ok, &err_t, &cd);
            if (cd) *conn_bad = true;
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? res : shd::object(err_t));
            return results;
        });

    return proxy;
}
}  // namespace

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
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
    shd::state_view lua(L);

    // Build the callable namespace shield.database.postgresql.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table database =
        shield::plugins::get_or_create_subtable(shield, "database");

    shd::object existing = database["postgresql"];
    if (!existing.is<shd::table>()) {
        auto* owner = reinterpret_cast<pgsql_instance*>(self);
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
        database["postgresql"] = ns;
    }

    return 0;
}
