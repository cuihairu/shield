// [SHIELD_PLUGIN] database.sqlite — Lua facade: the per-instance
// proxy (sync query/query_one/execute/transaction plus the
// submit-and-suspend async shim), the per-call tx handle proxy and
// register_lua.
// Split out of shield_db_sqlite.cpp.
#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
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
#include "shield_db_sqlite_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

namespace {
// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.database.* facade.
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}
}  // namespace

namespace {
// Bind one Lua value onto a prepared statement at `idx` (1-based). Mapping:
//   nil/invalid      -> NULL
//   bool             -> INTEGER (0/1)
//   lua_Integer      -> INTEGER64 (covers Lua 5.3+ native ints)
//   double           -> FLOAT
//   string           -> TEXT
//   anything else    -> NULL (with a soft warning in *err_msg if provided)
int bind_lua_param(sqlite3_stmt* stmt, int idx, const shd::object& v,
                   std::string* err_msg) {
    if (!v.valid() || v == shd::nil) {
        return sqlite3_bind_null(stmt, idx);
    }
    // Boolean first — sol casts bool to int otherwise.
    if (v.is<bool>()) {
        bool b = v.as<bool>();
        return sqlite3_bind_int(stmt, idx, b ? 1 : 0);
    }
    if (v.is<lua_Integer>()) {
        // Covers Lua 5.3+ native 64-bit integers (including values > INT_MAX).
        return sqlite3_bind_int64(stmt, idx, v.as<lua_Integer>());
    }
    if (v.is<double>()) {
        return sqlite3_bind_double(stmt, idx, v.as<double>());
    }
    if (v.is<std::string>()) {
        std::string s = v.as<std::string>();
        return sqlite3_bind_text(stmt, idx, s.c_str(),
                                 static_cast<int>(s.size()), SQLITE_TRANSIENT);
    }
    // Fallback: bind NULL for anything we can't convert (tables, functions,
    // userdata). This avoids silent mis-binding; caller sees a soft warning.
    if (err_msg)
        *err_msg = "unsupported parameter type at index " +
                   std::to_string(idx) + "; bound as NULL";
    return sqlite3_bind_null(stmt, idx);
}
}  // namespace

namespace {
// Convert the current row of `stmt` into a Lua table keyed by column name.
shd::table row_to_lua(shd::state_view lua, sqlite3_stmt* stmt) {
    auto row = lua.create_table();
    int n = sqlite3_column_count(stmt);
    for (int c = 0; c < n; ++c) {
        const char* name = sqlite3_column_name(stmt, c);
        switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_INTEGER:
                row[name] =
                    static_cast<lua_Integer>(sqlite3_column_int64(stmt, c));
                break;
            case SQLITE_FLOAT:
                row[name] = sqlite3_column_double(stmt, c);
                break;
            case SQLITE_TEXT: {
                const unsigned char* t = sqlite3_column_text(stmt, c);
                // Split into branches: a ternary of std::string vs shd::nil
                // is ambiguous under the facade.
                if (t) {
                    row[name] = std::string(reinterpret_cast<const char*>(t));
                } else {
                    row[name] = shd::nil;
                }
                break;
            }
            case SQLITE_BLOB: {
                const void* b = sqlite3_column_blob(stmt, c);
                int sz = sqlite3_column_bytes(stmt, c);
                if (b) {
                    row[name] = std::string(static_cast<const char*>(b),
                                            static_cast<size_t>(sz));
                } else {
                    row[name] = shd::nil;
                }
                break;
            }
            default:
                row[name] = shd::nil;  // SQLITE_NULL
                break;
        }
    }
    return row;
}
}  // namespace

namespace {
// Prepare + bind + step a statement on the given connection. Used by the
// proxy query/query_one/execute helpers. The `mode` argument selects the
// return shape. On error, *ok is set to false and *err_out receives an error
// table.
//
// Returns one of:
//   query        -> sequence table {row1, row2, ...}
//   query_one    -> single row table or nil
//   execute      -> table {affected=N, last_insert_id=M}
shd::object run_statement(
    shd::state_view lua, sqlite3* db, const std::string& sql,
    std::optional<shd::table> params,
    const char* mode,  // "query" | "query_one" | "execute"
    bool* ok, shd::table* err_out) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        *ok = false;
        *err_out =
            make_error_table(lua, map_sqlite_error(rc), sqlite3_errmsg(db));
        return shd::nil;
    }
    // Bind parameters (Lua sequence table 1..N). Collect positional values
    // first to avoid any iterator invalidation from stack work during bind.
    if (params && params->valid()) {
        std::vector<shd::object> positional;
        for (const auto& kv : *params) {
            if (kv.first.get_type() != shd::type::number) continue;
            positional.push_back(kv.second);
        }
        for (size_t i = 0; i < positional.size(); ++i) {
            std::string bind_err;
            int brc = bind_lua_param(stmt, static_cast<int>(i + 1),
                                     positional[i], &bind_err);
            if (brc != SQLITE_OK) {
                sqlite3_finalize(stmt);
                *ok = false;
                *err_out = make_error_table(
                    lua, map_sqlite_error(brc),
                    bind_err.empty() ? sqlite3_errmsg(db) : bind_err);
                return shd::nil;
            }
        }
    }

    shd::object result = shd::nil;
    if (std::strcmp(mode, "execute") == 0) {
        // Step once; we don't collect rows for DML.
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            *ok = false;
            *err_out =
                make_error_table(lua, map_sqlite_error(rc), sqlite3_errmsg(db));
            return shd::nil;
        }
        auto t = lua.create_table();
        t["affected"] = static_cast<lua_Integer>(sqlite3_changes(db));
        t["last_insert_id"] =
            static_cast<lua_Integer>(sqlite3_last_insert_rowid(db));
        result = t;
    } else if (std::strcmp(mode, "query_one") == 0) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            result = row_to_lua(lua, stmt);
        } else if (rc == SQLITE_DONE) {
            result = shd::object(shd::nil);  // no rows
        } else {
            sqlite3_finalize(stmt);
            *ok = false;
            *err_out =
                make_error_table(lua, map_sqlite_error(rc), sqlite3_errmsg(db));
            return shd::nil;
        }
    } else {  // "query"
        auto rows = lua.create_table();
        int row_index = 1;
        while (true) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                rows[row_index++] = row_to_lua(lua, stmt);
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                sqlite3_finalize(stmt);
                *ok = false;
                *err_out = make_error_table(lua, map_sqlite_error(rc),
                                            sqlite3_errmsg(db));
                return shd::nil;
            }
        }
        result = rows;
    }
    sqlite3_finalize(stmt);
    *ok = true;
    return result;
}
}  // namespace

namespace {
// Build a per-call proxy table bound to a specific sqlite3* handle. Used by
// transaction() so the callback's tx:execute/tx:query share one connection.
shd::table make_handle_proxy(shd::state_view lua, sqlite3* db);

// Async shim installed over the instance proxy: query/query_one/execute
// become submit-and-yield wrappers around the __sync_* implementations.
// The shim source is shared with the mysql/postgresql drivers — see
// plugins/_shared/shield_db_async_shim.hpp for the contract.
constexpr const char* kAsyncShim = shield::plugins::kDbAsyncShimLua;
}  // namespace

namespace {
// Build a per-instance proxy table that opens a fresh connection per call.
shd::table make_instance_proxy(shd::state_view lua, sqlite_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "__sync_query",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            shd::table err;
            shd::object rows =
                run_statement(lua, db, sql, params, "query", &ok, &err);
            sqlite3_close(db);
            results.push_back(shd::make_object(lua, ok));
            if (ok) {
                results.push_back(rows);
            } else {
                results.push_back(err);
            }
            return results;
        });

    proxy.set_function(
        "__sync_query_one",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            shd::table err;
            shd::object row =
                run_statement(lua, db, sql, params, "query_one", &ok, &err);
            sqlite3_close(db);
            results.push_back(shd::make_object(lua, ok));
            if (ok) {
                results.push_back(row);  // row or nil
            } else {
                results.push_back(err);
            }
            return results;
        });

    proxy.set_function(
        "__sync_execute",
        [inst](shd::this_state s, std::string sql,
               std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            bool ok = false;
            shd::table err;
            shd::object res =
                run_statement(lua, db, sql, params, "execute", &ok, &err);
            sqlite3_close(db);
            results.push_back(shd::make_object(lua, ok));
            if (ok) {
                results.push_back(res);
            } else {
                results.push_back(err);
            }
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
            std::string open_err;
            sqlite3* db = open_connection(inst, &open_err);
            if (!db) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }

            // BEGIN
            char* begin_err = nullptr;
            if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, &begin_err) !=
                SQLITE_OK) {
                std::string msg = begin_err ? begin_err : "BEGIN failed";
                sqlite3_free(begin_err);
                sqlite3_close(db);
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "db_query_failed", msg));
                return results;
            }

            // tx proxy shares this db handle.
            shd::table tx = make_handle_proxy(lua, db);
            shd::protected_function_result cb_res = callback(tx);
            bool commit = cb_res.valid();
            bool user_abort = false;

            if (cb_res.valid()) {
                // A boolean `false` first return is treated as user-initiated
                // rollback (matches the host facade's contract).
                shd::object first = cb_res.get(0);
                if (first.is<bool>() && !first.as<bool>()) {
                    commit = false;
                    user_abort = true;
                }
            }

            const char* tx_sql = commit ? "COMMIT" : "ROLLBACK";
            char* tx_err = nullptr;
            int trc = sqlite3_exec(db, tx_sql, nullptr, nullptr, &tx_err);
            std::string tx_msg = tx_err ? tx_err : "";
            sqlite3_free(tx_err);

            if (trc != SQLITE_OK) {
                sqlite3_close(db);
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, map_sqlite_error(trc),
                    tx_msg.empty()
                        ? std::string(tx_sql) + std::string(" failed")
                        : tx_msg));
                return results;
            }

            sqlite3_close(db);

            if (!cb_res.valid()) {
                // Lua callback threw — report as soft failure after rollback.
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(lua,
                                                   "transaction_rolled_back",
                                                   "callback raised an error"));
                return results;
            }

            if (user_abort) {
                // Callback explicitly returned false — propagate its returns.
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

    // Async entry (docs/db-async-design.md M2): register the suspension via
    // the host primitive, then hand the task to the worker thread. Suspend
    // happens BEFORE enqueue so a fast worker completion always finds its
    // session (the host's yield-window guard handles the requeue). Returns
    // the session id, or 0 when the caller must run synchronously.
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
            sqlite_task task;
            task.kind = task_kind::stmt;
            const std::string tag = "db:sqlite:" + method;
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
                        task.params.push_back(
                            static_cast<int64_t>(v.as<lua_Integer>()));
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
            if (session == 0) {
                return 0;  // not suspendable here — caller runs sync
            }
            task.session = session;
            submit_async(inst, std::move(task));
            return session;
        });

    // Install the shim. Failure is impossible for static source, but a
    // fallback keeps the module usable synchronously if it ever trips.
    shd::load_result shim = lua.load(kAsyncShim, "=db_async_shim");
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
            inst->host_api->log(SHIELD_LOG_WARN, "database.sqlite",
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
}  // namespace

namespace {
// Proxy whose methods reuse a shared sqlite3* (used inside transactions).
shd::table make_handle_proxy(shd::state_view lua, sqlite3* db) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "query",
        [db](shd::this_state s, std::string sql,
             std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            shd::table err;
            shd::object rows =
                run_statement(lua, db, sql, params, "query", &ok, &err);
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? rows : shd::object(err));
            return results;
        });

    proxy.set_function(
        "query_one",
        [db](shd::this_state s, std::string sql,
             std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            shd::table err;
            shd::object row =
                run_statement(lua, db, sql, params, "query_one", &ok, &err);
            results.push_back(shd::make_object(lua, ok));
            results.push_back(ok ? row : shd::object(err));
            return results;
        });

    proxy.set_function(
        "execute",
        [db](shd::this_state s, std::string sql,
             std::optional<shd::table> params) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            bool ok = false;
            shd::table err;
            shd::object res =
                run_statement(lua, db, sql, params, "execute", &ok, &err);
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
    // shield.database.sqlite. Lua passes a binding logical name; PluginHost
    // resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "database.sqlite: lua_State is null";
        }
        return 1;
    }
    shd::state_view lua(L);

    // Build the callable namespace shield.database.sqlite.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table database =
        shield::plugins::get_or_create_subtable(shield, "database");

    shd::object existing = database["sqlite"];
    if (!existing.is<shd::table>()) {
        auto* owner = reinterpret_cast<sqlite_instance*>(self);
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
                // Attach the shared mapper / register_mapper / entity DSL.
                // Failure here is non-fatal — the proxy keeps its C++ methods.
                shield::plugins::apply_db_mapper_api(lua, proxy);
                results.push_back(shd::make_object(lua, proxy));
                return results;
            });
        shield::plugins::set_metatable(ns, mt);
        database["sqlite"] = ns;
    }

    return 0;
}
