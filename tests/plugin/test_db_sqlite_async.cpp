// DB-async M2 end-to-end (docs/db-async-design.md): the REAL sqlite plugin
// .so, started through a real PluginHost, serving a live service actor.
//
// Unlike the M1 primitive suite (which parked hand-built coroutines through
// C bridges), every suspension here goes through the plugin's Lua shim:
// db:query / db:query_one / db:execute submit to the plugin worker thread,
// the calling coroutine yields, and the worker's completion rides the host
// resume primitive back into the actor. Covered:
//   - async round trip with the service responsive while parked (timing)
//   - result shapes: query rows / query_one / execute / params / errors
//   - caller timeout -> {code="timeout"}; the late worker completion is
//     rejected (connection discarded) and the instance keeps working
//   - async: false -> the same call runs synchronously (no yield at all)
//   - PluginHost::shutdown drains an in-flight worker task before stopping
//
// Each VM picks the namespace up automatically: LuaRuntime::register_api
// runs global_host().register_lua_all on every service VM it creates.
#define BOOST_TEST_MODULE shield_db_sqlite_async
#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/plugin/plugin_host.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_db_sqlite_async";

// A recursive-CTE aggregate that keeps one sqlite worker busy for well over
// a tenth of a second but under a second locally in release (~650ms for 2M
// rows; the CTE is superlinear — 4M rows already takes ~5.6s), longer than
// the short instance's 150ms caller timeout. Cases that must observe the
// post-statement state wait on the pool gauges (pending_async / holding),
// never on a fixed sleep: a Debug sqlite3 runs this CTE several times
// slower, so wall-clock budgets would race the single worker ("timeout is
// not cancellation" means a timed-out caller's SQL keeps the worker busy to
// the end, and queued tasks wait for it).
const char* kSlowSql =
    "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c "
    "WHERE x < 2000000) SELECT sum(x) AS total FROM c";
// The recursion appends x+1 for every source row with x < 2000000, so the
// sequence runs 1..2000000: sum = n(n+1)/2 with n = 2000000.
const char* kSlowTotal = "2000001000000";

std::string opts_for(const std::string& name) {
    nlohmann::json opts = {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
    return opts.dump();
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

// ---------------------------------------------------------------------------
// PluginHost startup with the real database.sqlite package: manifest copied
// from the source tree, the CMake-built .so laid out under bin/. Four
// instances off the same package:
//   db.long   — 30s caller budget (queries finish before any timeout)
//   db.short  — 150ms caller budget (slow queries must time out)
//   db.shapes — file-backed (the shapes case needs cross-connection state)
//   db.off    — async disabled (sync fallback)
// ---------------------------------------------------------------------------
const shield_host_api_v1* ensure_sqlite_host() {
    static const shield_host_api_v1* api = nullptr;
    static bool tried = false;
    if (tried) return api;
    tried = true;

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(kTmpDir, ec);
    const fs::path plugins = kTmpDir + "/plugins";
    fs::remove_all(plugins, ec);
    const fs::path pkg = plugins / "database.sqlite";
    fs::create_directories(pkg / "bin", ec);
    // The manifest ships the real config schema (async / call_timeout_ms
    // included), so the instance configs below are validated for real.
    fs::copy_file(
        std::string(SHIELD_SOURCE_DIR) + "/plugins/sqlite/manifest.yaml",
        pkg / "manifest.yaml", fs::copy_options::overwrite_existing, ec);
    fs::copy_file(SHIELD_SQLITE_LIBRARY,
                  pkg / "bin" / "libshield_database_sqlite.so",
                  fs::copy_options::overwrite_existing, ec);

    shield::plugin::PluginConfig pcfg;
    pcfg.directory = plugins.string();
    auto add_instance = [&](const char* id, nlohmann::json config) {
        shield::plugin::InstanceDecl decl;
        decl.id = id;
        decl.package = "database.sqlite";
        decl.required = true;
        decl.config = std::move(config);
        pcfg.instances.push_back(std::move(decl));
    };
    add_instance("sqlite_async_long",
                 {{"database", ":memory:"}, {"call_timeout_ms", 30000}});
    add_instance("sqlite_async_short",
                 {{"database", ":memory:"}, {"call_timeout_ms", 150}});
    // The shapes scenario crosses per-call connections, so it needs a real
    // file (a :memory: database would forget the table between calls).
    // The timestamp suffix keeps reruns from tripping over a stale table.
    add_instance(
        "sqlite_async_shapes",
        {{"database",
          kTmpDir + "/shapes-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".db"},
         {"call_timeout_ms", 30000}});
    add_instance("sqlite_async_off",
                 {{"database", ":memory:"}, {"async", false}});
    auto add_binding = [&](const char* logical, const char* instance) {
        shield::plugin::BindingDecl b;
        b.logical = logical;
        b.instance_id = instance;
        pcfg.bindings.push_back(std::move(b));
    };
    add_binding("db.long", "sqlite_async_long");
    add_binding("db.short", "sqlite_async_short");
    add_binding("db.shapes", "sqlite_async_shapes");
    add_binding("db.off", "sqlite_async_off");

    std::string error;
    if (!shield::plugin::global_host().startup(pcfg, error)) {
        BOOST_TEST_MESSAGE("sqlite plugin startup failed: " << error);
        return api;
    }
    api = reinterpret_cast<const shield_host_api_v1*>(1);  // started marker
    return api;
}

// Evaluate `expr` on the service's main state until it returns Lua true.
// exec_lua stringifies tables, so every assertion is computed in Lua.
bool wait_until_lua(
    LuaServiceManager& manager, LuaRuntime& runtime,
    const std::string& service_id, const std::string& expr,
    std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
    const std::string sid = service_id;
    const std::string code = "return " + expr;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        std::atomic<bool> done{false};
        bool accepted = false;
        manager.enqueue_forked_task(sid, [&] {
            auto vm = manager.service_vm(sid);
            if (!vm) {
                done = true;
                return;
            }
            nlohmann::json r;
            std::string e;
            if (runtime.exec_lua(vm, code, &r, &e) && r.is_array() &&
                !r.empty() && r[0].is_boolean()) {
                accepted = r[0].get<bool>();
            }
            done = true;
        });
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (accepted) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Read a one-shot Lua boolean from the service main state.
bool lua_bool(LuaServiceManager& manager, LuaRuntime& runtime,
              const std::string& service_id, const std::string& expr) {
    std::atomic<bool> done{false};
    bool value = false;
    manager.enqueue_forked_task(service_id, [&] {
        auto vm = manager.service_vm(service_id);
        if (!vm) {
            done = true;
            return;
        }
        nlohmann::json r;
        std::string e;
        if (runtime.exec_lua(vm, "return " + expr, &r, &e) && r.is_array() &&
            !r.empty() && r[0].is_boolean()) {
            value = r[0].get<bool>();
        }
        done = true;
    });
    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return value;
}

// One-shot string probe (diagnostics: what a failed wait actually sees).
void lua_str(LuaServiceManager& manager, LuaRuntime& runtime,
             const std::string& service_id, const std::string& expr,
             std::string* out) {
    std::atomic<bool> done{false};
    manager.enqueue_forked_task(service_id, [&] {
        auto vm = manager.service_vm(service_id);
        if (vm) {
            nlohmann::json r;
            std::string e;
            if (runtime.exec_lua(vm, "return " + expr, &r, &e) &&
                r.is_array() && !r.empty() && r[0].is_string()) {
                *out = r[0].get<std::string>();
            }
        }
        done = true;
    });
    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Find one instance's pool-stats sample (nullptr when not collected). The
// returned pointer is only valid until the next call — callers re-poll.
const shield::plugin::PoolStatsResult* find_pool_stats(
    const std::string& instance_id) {
    static std::vector<shield::plugin::PoolStatsResult> scratch;
    scratch.clear();
    shield::plugin::global_host().collect_pool_stats(scratch);
    for (const auto& r : scratch) {
        if (r.instance_id == instance_id) return &r;
    }
    return nullptr;
}

// A coroutine parked inside the plugin shim, plus its cleanup plumbing.
struct ParkedCall {
    std::atomic<bool> parked{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> sync{false};  // completed without yielding
    int co_ref = LUA_NOREF;
};

// Diagnostics: last park failure reason (set on the actor thread).
std::string g_park_error;

const char* lua_status_name(int status) {
    switch (status) {
        case LUA_OK:
            return "LUA_OK";
        case LUA_YIELD:
            return "LUA_YIELD";
        case LUA_ERRRUN:
            return "LUA_ERRRUN";
        case LUA_ERRSYNTAX:
            return "LUA_ERRSYNTAX";
        case LUA_ERRMEM:
            return "LUA_ERRMEM";
        case LUA_ERRERR:
            return "LUA_ERRERR";
        default:
            return "OTHER";
    }
}

// The body executed inside the parked coroutine. The chunk is loaded (not
// executed) and handed to the coroutine, so it takes (db, sql) as varargs
// and runs its statements directly — no factory wrapper.
const char* kQueryBody = R"lua(
local db, sql = ...
_G.__db_out = nil
local ok, r = db:query(sql)
if ok then
    _G.__db_out = {ok = true, n = #r, first = r[1]}
else
    _G.__db_out = {ok = false, code = r and r.code or nil,
                   retryable = r and r.retryable or nil}
end
)lua";

// Run `body` (a chunk taking (db, sql) as varargs) in a fresh coroutine on
// the service actor. Returns as soon as the coroutine has yielded into the
// shim (or fallen back to sync). binding, body and sql are copied by value:
// the task outlives this frame.
void park_body(LuaServiceManager& manager, LuaRuntime& runtime,
               const std::string& service_id, const std::string& binding,
               const std::string& body, const std::string& sql,
               ParkedCall& out) {
    const std::string sid = service_id;
    const std::string b = binding;
    const std::string q = sql;
    (void)manager.enqueue_forked_task(sid, [&, sid, b, q, body] {
        g_park_error.clear();  // diagnostics report THIS park, not a stale one
        auto vm = manager.service_vm(sid);
        if (!vm) {
            g_park_error = "service_vm null";
            out.failed = true;
            return;
        }
        lua_State* main_L = runtime.vm_main_state(vm);
        const std::string setup =
            "local db = assert(shield.database.sqlite('" + b + "')) return db";
        if (luaL_dostring(main_L, setup.c_str()) != LUA_OK) {
            const char* e = lua_tostring(main_L, -1);
            g_park_error =
                "setup failed: " + std::string(e ? e : "non-string error");
            lua_pop(main_L, 1);
            out.failed = true;
            return;
        }
        // Stack: the proxy. Hand it plus the query body to a fresh coroutine.
        lua_State* co = lua_newthread(main_L);
        out.co_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);  // refs + pops co
        if (luaL_loadstring(main_L, body.c_str()) != LUA_OK) {
            const char* e = lua_tostring(main_L, -1);
            g_park_error = "body load failed: " + std::string(e ? e : "?");
            lua_pop(main_L, 1);
            out.failed = true;
            return;
        }
        // Order matters: the callable sits below its args on the coroutine
        // stack, so move the function first, then the proxy.
        lua_xmove(main_L, co, 1);  // body function (top of the main stack)
        lua_xmove(main_L, co, 1);  // then the proxy beneath it
        lua_pushlstring(co, q.data(), q.size());
        int nres = 0;
        // Stack: [fn, proxy, sql] — nargs counts the two ARGS; the callable
        // is the bottom value (exactly the M1 pattern).
        const int status = lua_resume(co, main_L, 2, &nres);
        // A sync completion (async off — the call never yields) lands here
        // as LUA_OK with the body already finished.
        if (status == LUA_OK) out.sync = true;
        if (status != LUA_YIELD) {
            const char* e = lua_tostring(co, -1);
            g_park_error = "resume status " +
                           std::string(lua_status_name(status)) + ": " +
                           (e ? e : "non-string error");
            luaL_unref(main_L, LUA_REGISTRYINDEX, out.co_ref);
            out.co_ref = LUA_NOREF;
            out.failed = true;
            return;
        }
        out.parked = true;
    });
}

// Run `db:query(sql)` in a parked coroutine (the M2 probe body).
void park_query(LuaServiceManager& manager, LuaRuntime& runtime,
                const std::string& service_id, const std::string& binding,
                const std::string& sql, ParkedCall& out) {
    park_body(manager, runtime, service_id, binding, kQueryBody, sql, out);
}

void unref_parked(LuaServiceManager& manager, LuaRuntime& runtime,
                  const std::string& service_id, ParkedCall& call) {
    if (call.co_ref == LUA_NOREF) return;
    const int ref = call.co_ref;
    call.co_ref = LUA_NOREF;
    const std::string sid = service_id;
    (void)manager.enqueue_forked_task(sid, [&manager, &runtime, sid, ref] {
        auto vm = manager.service_vm(sid);
        if (!vm) return;
        luaL_unref(runtime.vm_main_state(vm), LUA_REGISTRYINDEX, ref);
    });
}

struct ServiceFixture {
    caf::actor_system_config cfg;
    caf::actor_system system{cfg};
    LuaRuntime runtime;
    LuaServiceManager manager{runtime, system};
    std::string service_id;

    explicit ServiceFixture(const std::string& name) {
        const std::string module = kTmpDir + "/" + name + ".lua";
        std::ofstream out(module, std::ios::trunc);
        out << "return {}\n";
        out.close();
        auto svc = manager.spawn(module, opts_for(name));
        BOOST_REQUIRE(svc.success);
        service_id = svc.service_id;
    }

    ~ServiceFixture() {
        if (!service_id.empty()) manager.exit(service_id, "test done");
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The core acceptance: a real query suspends the calling coroutine through
// the plugin shim; the service keeps dispatching while the worker runs; the
// completion values arrive through the resume primitive.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncQueryRoundTripKeepsServiceResponsive) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_roundtrip");

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, "db.long", kSlowSql,
               call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());
    BOOST_CHECK(!call.sync.load());

    // While the query is in flight the same service still dispatches fork
    // tasks — the timing assertion the sync driver could never pass. The
    // side task and the completion resume are both actor tasks in FIFO
    // order, and the completion cannot even exist until the worker finishes
    // the SQL, so "side task ran before the result appeared" holds no
    // matter how fast the machine is.
    std::atomic<bool> side_ran{false};
    fx.manager.enqueue_forked_task(fx.service_id, [&] { side_ran = true; });

    // The worker completes and the values ride the resume path back.
    BOOST_REQUIRE(wait_until_lua(
        fx.manager, fx.runtime, fx.service_id,
        std::string("type(_G.__db_out) == 'table' and _G.__db_out.ok == true "
                    "and _G.__db_out.n == 1 and "
                    "type(_G.__db_out.first) == 'table' and "
                    "_G.__db_out.first.total == ") +
            kSlowTotal,
        std::chrono::milliseconds(30000)));

    // The service dispatched the side fork task while the call was suspended
    // (guaranteed by FIFO order — see the enqueue above).
    BOOST_CHECK(side_ran.load());

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// Result shapes across the three entry points, both call syntaxes, bound
// params, NULL columns and the error table — all through async round trips
// inside one parked coroutine (each statement suspends once).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncShapesQueryOneExecuteParamsAndErrors) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_shapes");

    // Everything below runs inside the parked coroutine; each db call
    // suspends, the worker completes it, and the host resumes the body.
    // The chunk is loaded (not executed): it takes the proxy as vararg and
    // runs directly.
    const char* body = R"lua(
local db = ...
_G.__shape_ok = false
_G.__shape_fail = nil
local function run()
    local ok, res = db:execute('CREATE TABLE cov(k INTEGER PRIMARY KEY, v TEXT)')
    if not ok then error('create: ' .. tostring(res and res.message)) end
    if res.affected ~= 0 then error('create affected') end

    ok, res = db:execute('INSERT INTO cov(k, v) VALUES (1, ?)', {'hello'})
    if not ok then error('insert1: ' .. tostring(res and res.message)) end
    if res.affected ~= 1 then error('insert1 affected') end
    if res.last_insert_id ~= 1 then error('insert1 id') end

    ok, res = db:execute('INSERT INTO cov(k, v) VALUES (2, ?)', {'world'})
    if not ok then error('insert2') end
    if res.last_insert_id ~= 2 then error('insert2 id') end

    ok, res = db:query('SELECT v FROM cov WHERE k = ?', {1})
    if not ok then error('q1: ' .. tostring(res and res.message)) end
    if #res ~= 1 or res[1].v ~= 'hello' then error('q1 shape') end

    -- dot-call shape must work identically to the colon form
    ok, res = db.query('SELECT v FROM cov WHERE k = ?', {2})
    if not ok then error('qdot') end
    if res[1].v ~= 'world' then error('qdot shape') end

    ok, res = db:query_one('SELECT v FROM cov WHERE k = ?', {2})
    if not ok or res == nil or res.v ~= 'world' then error('qone') end

    ok, res = db:query_one('SELECT v FROM cov WHERE k = ?', {42})
    if not ok or res ~= nil then error('qone miss should be nil') end

    ok, res = db:query_one('SELECT NULL AS n')
    if not ok or res == nil or res.n ~= nil then error('null col') end

    ok, res = db:query('SELEC nope')
    if ok or type(res) ~= 'table' or res.code == nil or res.message == nil then
        error('bad sql must return (false, {code, message})')
    end

    ok, res = db:query('SELECT v FROM cov WHERE missing = ?', {'x'})
    if ok or type(res) ~= 'table' or res.code == nil then
        error('bind-error must return (false, {code, message})')
    end
end
local ok, err = pcall(run)
if not ok then
    _G.__shape_fail = tostring(err)
else
    _G.__shape_ok = true
end
)lua";

    std::atomic<bool> done{false};
    ParkedCall call;
    const std::string sid = fx.service_id;
    (void)fx.manager.enqueue_forked_task(sid, [&, sid] {
        auto vm = fx.manager.service_vm(sid);
        if (!vm) {
            done = true;
            return;
        }
        lua_State* main_L = fx.runtime.vm_main_state(vm);
        const char* setup =
            "local db = assert(shield.database.sqlite('db.shapes')) return db";
        if (luaL_dostring(main_L, setup) != LUA_OK) {
            lua_pop(main_L, 1);
            done = true;
            return;
        }
        lua_State* co = lua_newthread(main_L);
        call.co_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);
        if (luaL_loadstring(main_L, body) != LUA_OK) {
            lua_pop(main_L, 1);
            done = true;
            return;
        }
        lua_xmove(main_L, co, 1);  // body function
        lua_xmove(main_L, co, 1);  // proxy (arg 1)
        int nres = 0;
        const int status = lua_resume(co, main_L, 1, &nres);  // fn(proxy)
        if (status != LUA_YIELD) {
            luaL_unref(main_L, LUA_REGISTRYINDEX, call.co_ref);
            call.co_ref = LUA_NOREF;
            done = true;
            return;
        }
        call.parked = true;
        done = true;
    });
    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    BOOST_REQUIRE(call.parked.load());

    BOOST_CHECK(wait_until_lua(
        fx.manager, fx.runtime, fx.service_id,
        "_G.__shape_ok == true or type(_G.__shape_fail) == 'string'",
        std::chrono::milliseconds(30000)));
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "_G.__shape_ok == true"));
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "tostring(_G.__shape_fail) == 'nil'"));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// Caller timeout: the short instance's 150ms budget expires while the slow
// query runs; the caller gets {code="timeout", retryable=true}; the late
// worker completion is rejected (discarded, never delivered), and the
// instance keeps serving afterwards.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncTimeoutPoisonsAndInstanceKeepsWorking) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_timeout");

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, "db.short", kSlowSql,
               call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());

    BOOST_REQUIRE(wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                                 "type(_G.__db_out) == 'table' and "
                                 "_G.__db_out.ok == false and "
                                 "_G.__db_out.code == 'timeout' and "
                                 "_G.__db_out.retryable == true"));

    // Let the worker finish the query and attempt the late completion; it
    // must be dropped, not delivered over the timeout result. The drain is
    // observed (pending_async back to 0), never timed: the statement's
    // runtime varies with build and load (a Debug sqlite3 runs this CTE
    // several times slower than release), so a fixed sleep here would race
    // the worker and hand the follow-up query below its own timeout while
    // the worker is still clogged.
    BOOST_CHECK(wait_until(
        [&] {
            const auto* s = find_pool_stats("sqlite_async_short");
            return s && s->status == shield::plugin::PoolStatsStatus::ok &&
                   s->stats.pending_async == 0;
        },
        std::chrono::milliseconds(30000)));
    // The late completion ran and was rejected: the timeout payload is
    // still what the caller saw.
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "type(_G.__db_out) == 'table' and "
                         "_G.__db_out.ok == false and "
                         "_G.__db_out.code == 'timeout'"));

    // Poisoned-connection handling never wedges the instance: a fast query
    // right after still round-trips.
    ParkedCall after;
    park_query(fx.manager, fx.runtime, fx.service_id, "db.short",
               "SELECT 41 AS v", after);
    BOOST_CHECK(wait_until([&] { return after.parked || after.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(after.parked.load());
    BOOST_CHECK(wait_until_lua(
        fx.manager, fx.runtime, fx.service_id,
        "type(_G.__db_out) == 'table' and _G.__db_out.ok == true and "
        "_G.__db_out.n == 1 and _G.__db_out.first.v == 41"));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
    unref_parked(fx.manager, fx.runtime, fx.service_id, after);
}

// ---------------------------------------------------------------------------
// async: false — the very same call inside a coroutine completes on the
// FIRST resume (no yield, no worker), which is the documented sync fallback.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncFalseRunsSynchronouslyInsideCoroutine) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_off");

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, "db.off", "SELECT 7 AS v",
               call);
    BOOST_CHECK(
        wait_until([&] { return call.parked || call.failed || call.sync; },
                   std::chrono::milliseconds(5000)));
    // Not parked: the coroutine ran to completion inside the first resume.
    BOOST_CHECK(call.sync.load());
    BOOST_CHECK(!call.parked.load());
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "type(_G.__db_out) == 'table' and "
                         "_G.__db_out.ok == true and _G.__db_out.n == 1 and "
                         "_G.__db_out.first.v == 7"));
}

// ---------------------------------------------------------------------------
// M4: the async transaction. One parked coroutine drives a full tx over the
// __tx_* protocol — commit visibility, rollback on explicit false, rollback
// on body error, the hard rule's runtime teeth (pool-level call and nested
// transaction inside a body raise), and forwarded return values. Everything
// crosses the suspend/resume boundary; the file-backed shapes instance makes
// post-tx visibility observable from separate per-call connections.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncTransactionCommitRollbackAndHardRules) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_tx");

    const char* body = R"lua(
local db = ...
local T = {}
_G.__tx_out = T
db:execute('DROP TABLE IF EXISTS txm')
local ok, err = db:execute('CREATE TABLE txm(k INTEGER PRIMARY KEY, v TEXT)')
if not ok then T.setup = err and err.code return end
T.setup = 'ok'

-- commit: the body sees its own rows; after COMMIT so does everyone else
ok, err = db:transaction(function(tx)
    local iok = tx:execute('INSERT INTO txm(k, v) VALUES (1, ?)', {'a'})
    if not iok then return false end
    local qok, qres = tx:query('SELECT COUNT(*) AS n FROM txm')
    if not qok then return false end
    if qres[1].n ~= 1 then return false end
    return true
end)
T.commit_ok = ok
-- On success the tx forwards the body's return values, so `err` here is the
-- body's `true` — only index it when it actually is an error table.
T.commit_err = (type(err) == 'table' and err.code) or nil
local cok, cres = db:query_one('SELECT COUNT(*) AS n FROM txm')
T.after_commit = cok and cres and cres.n or -1

-- explicit false -> user rollback
ok, err = db:transaction(function(tx)
    tx:execute('INSERT INTO txm(k, v) VALUES (2, ?)', {'b'})
    return false
end)
T.rb_false_ok = ok
T.rb_false_code = (type(err) == 'table' and err.code) or nil
local fok, fres = db:query_one('SELECT COUNT(*) AS n FROM txm')
T.after_rb_false = fok and fres and fres.n or -1

-- body error -> rollback
ok, err = db:transaction(function(tx)
    tx:execute('INSERT INTO txm(k, v) VALUES (3, ?)', {'c'})
    error('boom')
end)
T.rb_err_ok = ok
T.rb_err_code = (type(err) == 'table' and err.code) or nil
local eok, eres = db:query_one('SELECT COUNT(*) AS n FROM txm')
T.after_rb_err = eok and eres and eres.n or -1

-- hard rule: pool-level call and nested transaction inside a body raise.
-- Both raise BEFORE any yield, so a plain pcall catches them.
ok = db:transaction(function(tx)
    local pok, perr = pcall(db.query, db, 'SELECT 1 AS v')
    T.pool_raised = not pok
    T.pool_msg = tostring(perr)
    local nok, nerr = pcall(db.transaction, db, function() return true end)
    T.nested_raised = not nok
    T.nested_msg = tostring(nerr)
    return false
end)
T.hard_rule_tx_ok = ok

-- success forwards the body's return values
local okf, ret = db:transaction(function(tx) return 'ret' end)
T.fwd_ok = okf
T.fwd_val = tostring(ret)
T.done = true
)lua";

    ParkedCall call;
    park_body(fx.manager, fx.runtime, fx.service_id, "db.shapes", body, "",
              call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());

    BOOST_CHECK(wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                               "_G.__tx_out and _G.__tx_out.done == true",
                               std::chrono::milliseconds(30000)));
    // exec_lua stringifies tables, so each field is asserted in Lua directly.
    std::string probe;
    {
        std::atomic<bool> done{false};
        fx.manager.enqueue_forked_task(fx.service_id, [&] {
            auto vm = fx.manager.service_vm(fx.service_id);
            if (vm) {
                nlohmann::json r;
                std::string e;
                if (fx.runtime.exec_lua(
                        vm,
                        "(function() local t = _G.__tx_out or {} local s = {} "
                        "for k, v in pairs(t) do s[#s + 1] = k .. '=' .. "
                        "tostring(v) end return table.concat(s, ' ') end)()",
                        &r, &e) &&
                    r.is_array() && !r.empty() && r[0].is_string()) {
                    probe = r[0].get<std::string>();
                }
            }
            done = true;
        });
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    BOOST_TEST_MESSAGE("tx out: [" << probe << "]");
    auto f = [&](const std::string& expr) {
        return lua_bool(fx.manager, fx.runtime, fx.service_id, expr);
    };
    BOOST_CHECK(f("_G.__tx_out.setup == 'ok'"));
    BOOST_CHECK(f("_G.__tx_out.commit_ok == true"));
    BOOST_CHECK(f("tonumber(_G.__tx_out.after_commit) == 1"));
    BOOST_CHECK(f("_G.__tx_out.rb_false_ok == false"));
    BOOST_CHECK(f("_G.__tx_out.rb_false_code == 'transaction_rolled_back'"));
    BOOST_CHECK(f("tonumber(_G.__tx_out.after_rb_false) == 1"));
    BOOST_CHECK(f("_G.__tx_out.rb_err_ok == false"));
    BOOST_CHECK(f("_G.__tx_out.rb_err_code == 'transaction_rolled_back'"));
    BOOST_CHECK(f("tonumber(_G.__tx_out.after_rb_err) == 1"));
    BOOST_CHECK(f("_G.__tx_out.pool_raised == true"));
    BOOST_CHECK(
        f("_G.__tx_out.pool_msg and "
          "_G.__tx_out.pool_msg:find('forbidden', 1, true) ~= nil"));
    BOOST_CHECK(f("_G.__tx_out.nested_raised == true"));
    BOOST_CHECK(
        f("_G.__tx_out.nested_msg and "
          "_G.__tx_out.nested_msg:find('forbidden', 1, true) ~= nil"));
    BOOST_CHECK(f("_G.__tx_out.hard_rule_tx_ok == false"));
    BOOST_CHECK(f("_G.__tx_out.fwd_ok == true"));
    BOOST_CHECK(f("_G.__tx_out.fwd_val == 'ret'"));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// M4 gauges: while a transaction body is parked on the slow statement the
// instance reports pending_async == 1 and holding == 1 (the open tx's handle
// is checked out); sqlite has no pool, so the pool gauges stay -1. Both
// return to 0 once the tx commits.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncTransactionGaugesWhileParked) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_tx_gauge");

    const char* body = R"lua(
local db, sql = ...
_G.__tx_gauge = nil
local ok, err = db:transaction(function(tx)
    local qok = tx:query(sql)
    return qok
end)
if ok then
    _G.__tx_gauge = 'ok'
else
    _G.__tx_gauge = 'fail:' .. tostring(err and err.code)
end
)lua";

    ParkedCall call;
    park_body(fx.manager, fx.runtime, fx.service_id, "db.long", body, kSlowSql,
              call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());

    // After BEGIN completes the tx holds its handle for the slow statement's
    // full ~650ms — a wide, pollable window.
    const bool gauges_seen = wait_until(
        [&] {
            const auto* s = find_pool_stats("sqlite_async_long");
            return s && s->status == shield::plugin::PoolStatsStatus::ok &&
                   s->stats.pending_async == 1 && s->stats.holding == 1;
        },
        std::chrono::milliseconds(5000));
    if (!gauges_seen) {
        if (const auto* s = find_pool_stats("sqlite_async_long")) {
            BOOST_TEST_MESSAGE("gauge probe: pending_async="
                               << s->stats.pending_async
                               << " holding=" << s->stats.holding);
        } else {
            BOOST_TEST_MESSAGE("gauge probe: instance not collected");
        }
    }
    BOOST_CHECK(gauges_seen);
    // No pool on sqlite: the pool gauges hold the -1 sentinel.
    if (const auto* s = find_pool_stats("sqlite_async_long")) {
        BOOST_CHECK_EQUAL(s->stats.size, -1);
        BOOST_CHECK_EQUAL(s->stats.in_use, -1);
    }

    const bool committed = wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                                          "_G.__tx_gauge == 'ok'",
                                          std::chrono::milliseconds(30000));
    if (!committed) {
        std::string probe;
        lua_str(fx.manager, fx.runtime, fx.service_id,
                "tostring(_G.__tx_gauge)", &probe);
        BOOST_TEST_MESSAGE("tx gauge probe: [" << probe << "]");
    }
    BOOST_REQUIRE(committed);
    BOOST_CHECK(wait_until(
        [&] {
            const auto* s = find_pool_stats("sqlite_async_long");
            return s && s->stats.pending_async == 0 && s->stats.holding == 0;
        },
        std::chrono::milliseconds(5000)));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// M4 caller timeout inside a tx body: the 150ms budget expires mid-statement;
// the body sees the timeout error, returns false, and the shim queues the
// rollback behind the still-running statement on the single worker. The
// rollback call itself exceeds its budget too, so the tx surfaces the timeout
// error — and once the statement finishes, the queued rollback runs anyway
// and both gauges drain to 0 (the rejected-resume cleanup leaves no leak).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncTransactionCallerTimeoutRollsBack) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_tx_to");

    const char* body = R"lua(
local db, sql = ...
_G.__tx_to = nil
local ok, err = db:transaction(function(tx)
    tx:execute('CREATE TABLE IF NOT EXISTS to_t(k INTEGER)')
    tx:execute('INSERT INTO to_t VALUES (1)')
    local qok = tx:query(sql)
    if not qok then return false end
    return true
end)
if ok then
    _G.__tx_to = 'committed'
else
    _G.__tx_to = 'fail:' .. tostring(err and err.code)
end
)lua";

    ParkedCall call;
    park_body(fx.manager, fx.runtime, fx.service_id, "db.short", body, kSlowSql,
              call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());

    // The tx is open and holding a handle while the slow statement runs.
    BOOST_CHECK(wait_until(
        [&] {
            const auto* s = find_pool_stats("sqlite_async_short");
            return s && s->stats.holding >= 1;
        },
        std::chrono::milliseconds(2000)));

    // Timeout mid-body -> rollback attempt -> that too times out (the slow
    // statement still owns the worker), so the tx surfaces the timeout.
    const bool got_timeout = wait_until_lua(
        fx.manager, fx.runtime, fx.service_id, "_G.__tx_to == 'fail:timeout'",
        std::chrono::milliseconds(5000));
    if (!got_timeout) {
        std::string probe;
        lua_str(fx.manager, fx.runtime, fx.service_id, "tostring(_G.__tx_to)",
                &probe);
        BOOST_TEST_MESSAGE("tx timeout probe: [" << probe << "]");
    }
    BOOST_REQUIRE(got_timeout);

    // The late statement finishes; the queued rollback then runs on the
    // worker (its completion is rejected — the caller is gone) and every
    // gauge drains.
    BOOST_CHECK(wait_until(
        [&] {
            const auto* s = find_pool_stats("sqlite_async_short");
            return s && s->stats.pending_async == 0 && s->stats.holding == 0;
        },
        std::chrono::milliseconds(10000)));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// PluginHost::shutdown while a worker task is in flight: the drain joins the
// worker before the instance dies — no hang, no use-after-free, and the
// already-completed timeout result is untouched by the late completion.
// Runs last: it stops the global plugin host.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ShutdownDrainsInFlightWorker) {
    if (!ensure_sqlite_host()) {
        BOOST_TEST_MESSAGE("sqlite plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx("sqlite_async_shutdown");

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, "db.short", kSlowSql,
               call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());

    // Caller budget (150ms) expires while the SQL keeps running.
    BOOST_REQUIRE(wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                                 "type(_G.__db_out) == 'table' and "
                                 "_G.__db_out.ok == false and "
                                 "_G.__db_out.code == 'timeout'"));

    // Shutdown joins the worker mid-query (~650ms) and stops the instance.
    const auto t0 = std::chrono::steady_clock::now();
    shield::plugin::global_host().shutdown();
    const auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
    BOOST_TEST_MESSAGE("shutdown drained in " << drain_ms << "ms");

    // Late completion dropped: the timeout payload is still what the caller
    // saw. (No DB calls after shutdown — the instance is stopped.)
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "type(_G.__db_out) == 'table' and "
                         "_G.__db_out.code == 'timeout'"));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}
