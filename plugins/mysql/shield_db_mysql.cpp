// [SHIELD_PLUGIN] database.mysql — MySQL provider for shield.database.v1.
//
// v1 ABI (shield_plugin_get_v1). The X DevAPI session lifecycle and SQL
// execution are inherited from the legacy implementation; only the ABI shell
// is new. Error code mapping follows the conventions used elsewhere in shield:
//   "connection_lost"      Lost connection / server gone
//   "connection_timeout"   query timed out
//   "syntax_error"         SQL parse / grammar error
//   "constraint_violation" Duplicate key / FK / CHECK
//   "transaction_aborted"  Deadlock
//   "db_query_failed"      Catch-all for other mysqlx::Error
//
// Lua autonomy: register_lua installs the shared callable namespace
// shield.database.mysql(binding). Each call acquires a Session from a
// per-instance connection pool, runs the statement, and returns the Session
// to the pool on scope exit. The C vtable still creates/closes a fresh
// Session per connect — C-ABI callers do NOT get pooling, only Lua callers
// do. This keeps the vtable semantics unchanged while giving Lua scripts the
// warm-connection performance they expect.

#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"
#include "shield/plugin/host_api.h"
#include "shield_db_mapper.hpp"
#include "shield_lua_plugin_binding.hpp"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <mysqlx/xdevapi.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
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

// shield_db_conn is forward-declared opaque in database.h; concrete layout
// defined here.
struct shield_db_conn {
    std::unique_ptr<mysqlx::Session> session;
};

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
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                return nullptr;
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0)
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                return nullptr;
            }
        },
        // disconnect
        [](shield_db_conn* c) {
            if (!c) return;
            if (c->session) { try { c->session->close(); } catch (...) {} }
            delete c;
        },
        // ping
        [](shield_db_conn* c) -> int {
            if (!c || !c->session) return 0;
            try {
                c->session->sql("SELECT 1").execute();
                return 1;
            } catch (...) { return 0; }
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
    int port = 33060;             // X Protocol port (NOT 3306)
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
    std::queue<std::shared_ptr<mysqlx::Session>> free_list;
    int current_size = 0;  // live sessions (in free_list + checked out)
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
// mysqlx::Session is expensive to construct (TCP + auth + session setup), so
// the Lua proxy keeps a per-instance free list. acquire_session() returns a
// RAII guard that pushes the Session back onto the free list (and notifies
// one waiter) when it goes out of scope.
//
// Lifecycle:
//   - Try free_list first (fast path, no construction).
//   - If free_list is empty and current_size < pool_size, open a new Session
//     (current_size is bumped under the lock so concurrent openers don't
//     overshoot).
//   - Otherwise wait on pool_cv up to acquire_timeout_ms, then fail.
//
// Broken sessions are discarded (not pushed back) so the next acquirer gets a
// fresh one. current_size is decremented when a session is dropped.
// ---------------------------------------------------------------------------

// Build a new mysqlx::Session from the instance config. Throws on failure.
std::shared_ptr<mysqlx::Session> open_session(const mysql_instance* inst) {
    // mysqlx::Session(host, port, user, password, schema) uses the X Protocol.
    // The query_timeout is applied per-statement via the SqlStatement API
    // (mysqlx has no per-session query timeout in the X DevAPI; the host
    // connect_timeout maps to the TCP connect phase implicitly).
    auto s = std::make_shared<mysqlx::Session>(
        inst->host,
        inst->port > 0 ? inst->port : 33060,
        inst->username.empty() ? std::string("root") : inst->username,
        inst->password,
        inst->database);
    return s;
}

// RAII guard — releases the session back to the pool on destruction. If the
// session is marked broken (e.g. the caller observed a connection error), the
// guard drops it instead of returning a known-bad session.
struct pool_guard {
    mysql_instance* inst = nullptr;
    std::shared_ptr<mysqlx::Session> sess;
    bool broken = false;

    pool_guard() = default;
    pool_guard(mysql_instance* i, std::shared_ptr<mysqlx::Session> s)
        : inst(i), sess(std::move(s)) {}

    ~pool_guard() {
        if (!inst || !sess) return;
        if (broken) {
            // Discard: try to close (best effort), then decrement live count.
            try { sess->close(); } catch (...) {}
            std::lock_guard lk(inst->pool_mu);
            inst->current_size -= 1;
            inst->pool_cv.notify_one();
            return;
        }
        std::lock_guard lk(inst->pool_mu);
        inst->free_list.push(std::move(sess));
        inst->pool_cv.notify_one();
    }

    pool_guard(const pool_guard&) = delete;
    pool_guard& operator=(const pool_guard&) = delete;
    pool_guard(pool_guard&& o) noexcept
        : inst(o.inst), sess(std::move(o.sess)), broken(o.broken) {
        o.inst = nullptr;
    }
    pool_guard& operator=(pool_guard&& o) noexcept {
        if (this != &o) {
            inst = o.inst;
            sess = std::move(o.sess);
            broken = o.broken;
            o.inst = nullptr;
        }
        return *this;
    }

    mysqlx::Session* operator->() const { return sess.get(); }
    mysqlx::Session& operator*() const { return *sess; }
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

    // Outcome enum for the in-lock probe: TAKE (free session), OPEN (slot
    // reserved, must construct outside the lock), WAIT (pool full).
    enum class Probe { take, open, wait };
    std::shared_ptr<mysqlx::Session> taken;
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
        return std::make_unique<pool_guard>(inst, std::move(taken));
    }

    if (probe == Probe::open) {
        try {
            return std::make_unique<pool_guard>(inst, open_session(inst));
        } catch (const std::exception& e) {
            if (err) *err = std::string("mysql connect: ") + e.what();
        } catch (...) {
            if (err) *err = "mysql connect: unknown error";
        }
        // Open failed — release the slot and wake one waiter.
        std::lock_guard lk(inst->pool_mu);
        inst->current_size -= 1;
        inst->pool_cv.notify_one();
        return std::make_unique<pool_guard>();
    }

    // probe == Probe::wait
    std::unique_lock lk(inst->pool_mu);
    bool got = inst->pool_cv.wait_for(lk,
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
        auto s = inst->free_list.front();
        inst->free_list.pop();
        lk.unlock();
        return std::make_unique<pool_guard>(inst, std::move(s));
    }
    // Slot opened up — reserve and open outside the lock.
    inst->current_size += 1;
    lk.unlock();
    try {
        return std::make_unique<pool_guard>(inst, open_session(inst));
    } catch (const std::exception& e) {
        if (err) *err = std::string("mysql connect: ") + e.what();
    } catch (...) {
        if (err) *err = "mysql connect: unknown error";
    }
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
        auto s = inst->free_list.front();
        inst->free_list.pop();
        try { s->close(); } catch (...) {}
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

// Convert one mysqlx::Value to a Lua object.
//
// We avoid switching on Value::getType() because the enum names differ across
// mysql-connector-cpp releases (e.g. VINT64 vs INT64 vs LONGLONG). Instead we
// probe with typed get<T>() calls inside try/catch blocks — mysqlx throws
// std::bad_cast (wrapped in mysqlx::Error) when the conversion is invalid, so
// the first successful extraction wins. The probe order prefers integer over
// double to preserve precision for BIGINT columns.
//
// Mapping:
//   null      -> nil
//   integer   -> lua_Integer (int64_t)
//   uint64    -> lua_Integer if it fits, else double
//   float/double/decimal -> number (Lua has no native decimal)
//   string    -> string
//   bytes     -> string (raw bytes)
//   anything else -> nil (we don't know the layout)
sol::object value_to_lua(sol::state_view lua, const mysqlx::Value& v) {
    if (v.isNull()) return sol::nil;

    // Integer family first (covers TINYINT/SMALLINT/INT/BIGINT/YEAR).
    try {
        return sol::make_object(lua,
            static_cast<lua_Integer>(v.get<int64_t>()));
    } catch (...) {}
    // Unsigned 64-bit (BIGINT UNSIGNED).
    try {
        uint64_t u = v.get<uint64_t>();
        if (u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return sol::make_object(lua,
                static_cast<lua_Integer>(static_cast<int64_t>(u)));
        }
        return sol::make_object(lua, static_cast<lua_Number>(
            static_cast<double>(u)));
    } catch (...) {}
    // Floating point (FLOAT/DOUBLE/DECIMAL).
    try {
        return sol::make_object(lua, v.get<double>());
    } catch (...) {}
    try {
        return sol::make_object(lua,
            static_cast<lua_Number>(static_cast<double>(v.get<float>())));
    } catch (...) {}
    // Boolean (BIT(1)/BOOL).
    try {
        return sol::make_object(lua, v.get<bool>());
    } catch (...) {}
    // String / raw bytes (CHAR/VARCHAR/TEXT/BLOB/ENUM/SET/DATE/TIME/DATETIME/
    // TIMESTAMP/JSON/GEOMETRY — mysqlx exposes them all as std::string via
    // the same conversion path).
    try {
        std::string s = v.get<std::string>();
        return sol::make_object(lua, std::move(s));
    } catch (...) {}
    return sol::nil;
}

// Convert a mysqlx::Row into a Lua table keyed by column name. Column names
// come from the SqlResult's Column metadata.
sol::table row_to_lua(sol::state_view lua,
                      const mysqlx::Row& row,
                      const mysqlx::Columns& cols) {
    auto t = lua.create_table();
    unsigned n = row.colCount();
    for (unsigned c = 0; c < n; ++c) {
        std::string name;
        try {
            name = cols[c].getColumnName();
        } catch (...) {
            name = std::to_string(c + 1);  // fallback 1-based key
        }
        t[name] = value_to_lua(lua, row[c]);
    }
    return t;
}

// Bind one Lua value onto a SqlStatement as a `?` placeholder. mysqlx accepts
// std::string, int64, double, bool, and a few others. nil binds NULL.
void bind_lua_param(mysqlx::SqlStatement& stmt, const sol::object& v) {
    if (!v.valid() || v == sol::nil) {
        stmt.bind(static_cast<const char*>(nullptr));
        return;
    }
    if (v.is<bool>()) {
        stmt.bind(v.as<bool>() ? 1 : 0);
        return;
    }
    // Integer first — sol2 will otherwise coerce to double.
    if (v.is<lua_Integer>()) {
        stmt.bind(static_cast<int64_t>(v.as<lua_Integer>()));
        return;
    }
    if (v.is<double>()) {
        stmt.bind(v.as<double>());
        return;
    }
    if (v.is<std::string>()) {
        stmt.bind(v.as<std::string>());
        return;
    }
    // Fallback: stringify (tables/functions/etc.) — this mirrors the sqlite
    // plugin's tolerant bind behaviour.
    try {
        std::string s = v.as<std::string>();
        stmt.bind(s);
    } catch (...) {
        stmt.bind(static_cast<const char*>(nullptr));
    }
}

// Collect positional params from a Lua table (sequence keys 1..N) in order.
// sol::table iteration order is unspecified, so we bucket by numeric key
// first and then sort by index before binding.
std::vector<sol::object> collect_positional(const sol::optional<sol::table>& params) {
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

// Run a statement on `session` and return one of:
//   "query"     -> sequence table {row1, row2, ...}
//   "query_one" -> single row table or nil
//   "execute"   -> table {affected=N, last_insert_id=M}
//
// On error, *ok is set to false and *err_out receives an error table. The
// `broken` flag is set when the error looks like a connection-lost so the
// caller can drop the pooled session.
sol::object run_statement(sol::state_view lua,
                          mysqlx::Session& session,
                          const std::string& sql,
                          const sol::optional<sol::table>& params,
                          const char* mode,  // "query"|"query_one"|"execute"
                          bool* ok, sol::table* err_out,
                          bool* broken) {
    try {
        auto stmt = session.sql(sql);
        for (auto& v : collect_positional(params)) {
            bind_lua_param(stmt, v);
        }
        mysqlx::SqlResult result = stmt.execute();

        if (std::strcmp(mode, "execute") == 0) {
            auto t = lua.create_table();
            t["affected"] = static_cast<lua_Integer>(
                result.getAffectedItemsCount());
            t["last_insert_id"] = static_cast<lua_Integer>(
                result.getAutoIncrementValue());
            *ok = true;
            return t;
        }

        mysqlx::Columns cols = result.getColumns();
        if (std::strcmp(mode, "query_one") == 0) {
            // mysqlx::SqlResult::count() reports the number of rows in the
            // result set. fetchOne() returns a default-constructed Row when
            // the result is empty — comparing against count() is the robust
            // way to distinguish "no rows" from "row whose first column is
            // SQL NULL".
            if (result.count() == 0) {
                *ok = true;
                return sol::nil;
            }
            mysqlx::Row row = result.fetchOne();
            *ok = true;
            return row_to_lua(lua, row, cols);
        }

        // "query" — sequence of rows.
        auto rows = lua.create_table();
        int idx = 1;
        for (const mysqlx::Row& row : result) {
            rows[idx++] = row_to_lua(lua, row, cols);
        }
        *ok = true;
        return rows;
    } catch (const mysqlx::Error& e) {
        *ok = false;
        std::string msg = e.what();
        const char* code = map_mysqlx_error(msg.c_str());
        // Mark broken for connection-class errors so the pool drops the session.
        if (broken) {
            *broken = (std::strcmp(code, "connection_lost") == 0 ||
                       std::strcmp(code, "connection_timeout") == 0);
        }
        *err_out = make_error_table(lua, code, msg);
        return sol::nil;
    } catch (const std::exception& e) {
        *ok = false;
        if (broken) *broken = false;
        *err_out = make_error_table(lua, "db_query_failed", e.what());
        return sol::nil;
    } catch (...) {
        *ok = false;
        if (broken) *broken = false;
        *err_out = make_error_table(lua, "db_query_failed",
                                    "unknown mysql error");
        return sol::nil;
    }
}

// Forward decl — make_handle_proxy is used by transaction().
sol::table make_handle_proxy(sol::state_view lua,
                             std::shared_ptr<mysqlx::Session> sess,
                             mysql_instance* inst);

// Build the per-instance proxy. Each top-level method acquires a Session from
// the pool (pool_guard returns it on scope exit).
sol::table make_instance_proxy(sol::state_view lua, mysql_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function("query",
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
            sol::object rows = run_statement(lua, *guard, sql, params,
                                             "query", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? rows : err);
            return results;
        });

    proxy.set_function("query_one",
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
            sol::object row = run_statement(lua, *guard, sql, params,
                                            "query_one", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? row : err);
            return results;
        });

    proxy.set_function("execute",
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
            sol::object res = run_statement(lua, *guard, sql, params,
                                            "execute", &ok, &err, &broken);
            guard->broken = broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? res : err);
            return results;
        });

    proxy.set_function("transaction",
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
            try {
                guard->sql("START TRANSACTION").execute();
            } catch (const mysqlx::Error& e) {
                guard->broken = true;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, map_mysqlx_error(e.what()), e.what()));
                return results;
            } catch (const std::exception& e) {
                guard->broken = true;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "db_query_failed", e.what()));
                return results;
            }

            // tx proxy shares this session.
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
            try {
                guard->sql(tx_sql).execute();
            } catch (const mysqlx::Error& e) {
                // COMMIT/ROLLBACK failed — typically connection lost or
                // deadlock during commit. Mark broken so the pool drops it.
                guard->broken = true;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, map_mysqlx_error(e.what()),
                    std::string(tx_sql) + ": " + e.what()));
                return results;
            } catch (const std::exception& e) {
                guard->broken = true;
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "db_query_failed",
                    std::string(tx_sql) + ": " + e.what()));
                return results;
            }

            if (!cb_res.valid()) {
                // Lua callback threw — we already rolled back.
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "transaction_rolled_back",
                    "callback raised an error"));
                return results;
            }

            if (user_abort) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "transaction_rolled_back",
                    "callback returned false"));
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

// Proxy whose methods reuse a shared mysqlx::Session (used inside
// transactions). Holds a shared_ptr so the session outlives the tx table.
sol::table make_handle_proxy(sol::state_view lua,
                             std::shared_ptr<mysqlx::Session> sess,
                             mysql_instance* /*inst*/) {
    auto proxy = lua.create_table();

    proxy.set_function("query",
        [sess](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object rows = run_statement(lua, *sess, sql, params,
                                             "query", &ok, &err, &broken);
            (void)broken;  // tx session lifecycle is owned by transaction()
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? rows : err);
            return results;
        });

    proxy.set_function("query_one",
        [sess](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object row = run_statement(lua, *sess, sql, params,
                                            "query_one", &ok, &err, &broken);
            (void)broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? row : err);
            return results;
        });

    proxy.set_function("execute",
        [sess](sol::this_state s, std::string sql,
               sol::optional<sol::table> params) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            bool ok = false;
            bool broken = false;
            sol::table err;
            sol::object res = run_statement(lua, *sess, sql, params,
                                            "execute", &ok, &err, &broken);
            (void)broken;
            results.push_back(sol::make_object(lua, ok));
            results.push_back(ok ? res : err);
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self,
                      struct lua_State* L,
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
        mt.set_function("__call",
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
                 shield_plugin_instance_v1** out,
                 shield_error_v1* err) {
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
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
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

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.mysql",
        "1.0.0",
        mysql_create,
    };
    return &abi;
}
