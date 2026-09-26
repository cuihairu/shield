// Coverage tests for the DB-async host primitives (docs/db-async-design.md
// M1): lua_suspend_current / lua_resume_session driven end-to-end through
// the host_api vtable against a live service actor.
//
// A tiny fake plugin (.so compiled at test time, the test_cov_lua_service
// covplug pattern) captures the host_api pointer at create() time — that is
// the only channel the vtable travels through. The Lua-side shim (suspend →
// yield → observe) is installed by the test itself as C closures bound to
// that pointer, exactly the shape a real plugin's register_lua produces.
#define BOOST_TEST_MODULE CovLuaDbAsync
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
#include <mutex>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <sstream>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/plugin/plugin_host.hpp"

#ifndef _WIN32
#include <dlfcn.h>
#endif

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_db_async";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

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
// Fake plugin: hands the host_api table out through an exported getter, so
// the test can drive the C vtable the same way a real DB plugin would.
// ---------------------------------------------------------------------------
const char* kDbPluginSource = R"FAKE(#include "shield/plugin/abi.h"
#include <cstring>

namespace {
const struct shield_host_api_v1* g_host = nullptr;

const void* no_iface(shield_plugin_instance_v1*, const char* name,
                     shield_error_v1*) {
    static const int dummy_vtable = 0;
    if (name && 0 == strcmp(name, "db.test.iface")) {
        return &dummy_vtable;
    }
    return nullptr;
}

int plain_lua(shield_plugin_instance_v1*, struct lua_State*,
              struct shield_error_v1*) {
    return 0;
}

int plain_start(shield_plugin_instance_v1*, shield_error_v1*) { return 0; }

int create(const struct shield_plugin_create_args_v1* args,
           struct shield_plugin_instance_v1** out, struct shield_error_v1*) {
    if (!args || !out) return -1;
    g_host = args->host_api;
    static struct shield_plugin_instance_v1 inst;
    inst.struct_size = (uint32_t)sizeof(inst);
    inst.get_interface = no_iface;
    inst.start = plain_start;
    inst.shutdown = nullptr;
    inst.register_lua = plain_lua;
    *out = &inst;
    return 0;
}
}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
dbplug_entry(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        (uint32_t)sizeof(struct shield_plugin_abi_v1), "dbplug.test", "1.0.0",
        create};
    return &abi;
}

extern "C" SHIELD_PLUGIN_EXPORT const void* dbplug_host_api(void) {
    return (const void*)g_host;
}
)FAKE";

// Compile the fake plugin and lay out its package directory. Returns an
// empty path when no compiler is available (the suite then skips).
std::filesystem::path build_db_plugin() {
    namespace fs = std::filesystem;
    const fs::path dir = kTmpDir + "/plugin";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "bin", ec);
    const fs::path src = dir / "dbplug.cpp";
    {
        std::ofstream out(src, std::ios::trunc);
        out << kDbPluginSource;
    }
    const fs::path so = dir / "bin" / "libdbplug.so";
    const fs::path inc = fs::path(SHIELD_SOURCE_DIR) / "include";
    const char* compilers[] = {"/usr/bin/x86_64-linux-gnu-g++-15",
                               "/usr/bin/g++", "/usr/bin/c++"};
    for (const char* cxx : compilers) {
        std::ostringstream cmd;
        cmd << cxx << " -std=c++17 -shared -fPIC -I\"" << inc.string()
            << "\" -o \"" << so.string() << "\" \"" << src.string() << "\"";
        if (std::system(cmd.str().c_str()) == 0 && fs::exists(so)) {
            const fs::path pkg = dir / "dbplug.test";
            fs::create_directories(pkg, ec);
            fs::copy_file(so, pkg / "libdbplug.so",
                          fs::copy_options::overwrite_existing, ec);
            fs::create_directories(pkg / "bin", ec);
            fs::copy_file(so, pkg / "bin" / "libdbplug.so",
                          fs::copy_options::overwrite_existing, ec);
            std::ofstream m(pkg / "manifest.yaml", std::ios::trunc);
            m << "schema_version: 1\n"
              << "id: dbplug.test\n"
              << "name: dbplug\n"
              << "version: 1.0.0\n"
              << "kind: coverage\n"
              << "entry: dbplug_entry\n"
              << "library:\n"
              << "  linux: bin/libdbplug.so\n"
              << "  macos: bin/libdbplug.so\n"
              << "  windows: bin/other.dll\n"
              << "provides:\n  - interface: db.test.iface\n"
              << "requires: []\n"
              << "config_schema:\n  type: object\n";
            return dir;
        }
    }
    return {};
}

// The host_api pointer, captured once via the fake plugin's create().
const shield_host_api_v1* ensure_db_plugin_host() {
    static const shield_host_api_v1* api = nullptr;
    static bool tried = false;
    if (tried) return api;
    tried = true;
    const auto root = build_db_plugin();
    if (root.empty()) return api;
    shield::plugin::PluginConfig pcfg;
    pcfg.directory = root.string();
    shield::plugin::InstanceDecl decl;
    decl.id = "dbplug_cov";
    decl.package = "dbplug.test";
    decl.required = true;
    pcfg.instances.push_back(decl);
    std::string error;
    if (!shield::plugin::global_host().startup(pcfg, error)) {
        BOOST_TEST_MESSAGE("dbplug startup failed: " << error);
        return api;
    }
#ifndef _WIN32
    // The host already mapped the .so; dlopen re-resolves the same image.
    void* handle = dlopen(
        (root / "dbplug.test" / "bin" / "libdbplug.so").c_str(), RTLD_NOW);
    if (handle) {
        using host_fn = const void* (*)();
        auto fn = reinterpret_cast<host_fn>(dlsym(handle, "dbplug_host_api"));
        if (fn) {
            api = static_cast<const shield_host_api_v1*>(fn());
        }
    }
#endif
    return api;
}

// ---------------------------------------------------------------------------
// Lua-side shim: the suspend → yield → observe shape the design doc's
// plugin shim follows. Installed per VM by the test as C closures bound to
// the captured host_api pointer.
// ---------------------------------------------------------------------------

std::atomic<uint64_t> g_last_session{0};

int db_suspend_bridge(lua_State* L) {
    auto* api = static_cast<const shield_host_api_v1*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (!api) {
        lua_pushinteger(L, 0);
        return 1;
    }
    const int timeout = static_cast<int>(luaL_checkinteger(L, 1));
    const char* tag = luaL_optstring(L, 2, "");
    const uint64_t session = api->lua_suspend_current(nullptr, L, timeout, tag);
    g_last_session.store(session);
    lua_pushinteger(L, static_cast<lua_Integer>(session));
    return 1;
}

int db_resume_bridge(lua_State* L) {
    auto* api = static_cast<const shield_host_api_v1*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (!api) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const auto session = static_cast<uint64_t>(luaL_checkinteger(L, 1));
    const int ok = lua_toboolean(L, 2);
    const char* json = luaL_optstring(L, 3, nullptr);
    lua_pushboolean(L,
                    api->lua_resume_session(nullptr, session, ok, json) == 0);
    return 1;
}

void install_bridges(lua_State* L) {
    const shield_host_api_v1* api = ensure_db_plugin_host();
    lua_getglobal(L, "__db_suspend");
    const bool installed = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (installed || !api) return;
    lua_pushlightuserdata(L, const_cast<shield_host_api_v1*>(api));
    lua_pushcclosure(L, db_suspend_bridge, 1);
    lua_setglobal(L, "__db_suspend");
    lua_pushlightuserdata(L, const_cast<shield_host_api_v1*>(api));
    lua_pushcclosure(L, db_resume_bridge, 1);
    lua_setglobal(L, "__db_resume");
}

const char* kShim = R"lua(return function(timeout, tag)
    _G.__out = nil
    local session = __db_suspend(timeout, tag)
    if session == 0 then
        _G.__out = {sync = true}
        return
    end
    local r = table.pack(coroutine.yield())
    _G.__out = {ok = (r[1] == true), n = r.n, first = r[2]}
end
)lua";

// A coroutine parked inside the shim, plus the plumbing to clean it up.
struct ParkedCall {
    std::mutex mu;
    uint64_t session = 0;
    bool parked = false;
    bool failed = false;
    int co_ref = LUA_NOREF;
};

// Park a coroutine on the service's actor: fork task → create thread →
// resume once → the shim suspends via lua_suspend_current and yields.
// service_id and tag are copied into the task: the task may run after this
// call returns. The caller relies only on `out` outliving the task, which
// holds because every user waits for parked/failed before moving on.
void park_coroutine(LuaServiceManager& manager, LuaRuntime& runtime,
                    const std::string& service_id, ParkedCall& out,
                    int timeout_ms, const char* tag) {
    const std::string sid = service_id;
    const std::string tag_str = tag;
    // By-value captures for the frame-local copies: the task runs on the
    // actor after this call has returned, so `[&]` on sid/tag_str/timeout_ms
    // would read dead stack memory (manager/runtime/out outlive the task —
    // callers wait for parked/failed before moving on).
    (void)manager.enqueue_forked_task(sid, [&, sid, tag_str, timeout_ms] {
        auto vm = manager.service_vm(sid);
        if (!vm) {
            out.failed = true;
            return;
        }
        lua_State* main_L = runtime.vm_main_state(vm);
        install_bridges(main_L);
        if (luaL_dostring(main_L, kShim) != LUA_OK) {
            lua_pop(main_L, 1);
            out.failed = true;
            return;
        }
        // Stack: the shim factory. Hand it to a fresh coroutine.
        lua_State* co = lua_newthread(main_L);
        out.co_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);  // refs + pops co
        lua_xmove(main_L, co, 1);                          // move the factory
        lua_pushinteger(co, timeout_ms);
        lua_pushstring(co, tag_str.c_str());
        int nres = 0;
        const int status = lua_resume(co, main_L, 2, &nres);
        if (status != LUA_YIELD) {
            // Sync fallback (main-thread refusal) or shim error.
            luaL_unref(main_L, LUA_REGISTRYINDEX, out.co_ref);
            out.co_ref = LUA_NOREF;
            out.failed = true;
            return;
        }
        std::lock_guard lock(out.mu);
        out.session = g_last_session.load();
        out.parked = true;
    });
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

// Evaluate `expr` on the service's main state (via the owner actor) until it
// returns Lua true. The expression reads _G.__out directly: exec_lua converts
// scalars but stringifies tables, so assertions must be computed in Lua.
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

}  // namespace

// ---------------------------------------------------------------------------
// Round trip: a coroutine suspends through the vtable; the service keeps
// processing other work while it is parked; a worker-thread resume delivers
// the completion values; a second resume for the same session is rejected.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RoundTripKeepsServiceResponsiveAndRejectsDoubleResume) {
    auto* api = ensure_db_plugin_host();
    if (!api) {
        BOOST_TEST_MESSAGE("db-async fixture unavailable; skipping");
        return;
    }

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("dbasync_mod.lua", "return {}\n");
    auto svc = manager.spawn(module, opts_for("dbasync_roundtrip_svc"));
    BOOST_REQUIRE(svc.success);

    ParkedCall call;
    park_coroutine(manager, runtime, svc.service_id, call, 10000,
                   "db:test:query");
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_REQUIRE(call.parked);
    BOOST_CHECK(call.session != 0);

    // While the coroutine is parked, the service still dispatches work —
    // the whole point of the async entry (a sync driver would hold the
    // actor through the suspend window).
    std::atomic<bool> side_task_ran{false};
    manager.enqueue_forked_task(svc.service_id, [&] { side_task_ran = true; });
    BOOST_CHECK(wait_until([&] { return side_task_ran.load(); },
                           std::chrono::milliseconds(5000)));

    // Worker-thread completion. The resume payload is the VALUES array only —
    // resume_suspended_caller pushes the ok boolean itself, so the shim sees
    // (true, {rows=[1,2,3]}) from this payload.
    BOOST_CHECK(api->lua_resume_session(nullptr, call.session, 1,
                                        R"([{"rows":[1,2,3]}])") == 0);

    BOOST_REQUIRE(wait_until_lua(
        manager, runtime, svc.service_id,
        "type(_G.__out) == 'table' and _G.__out.ok == true and "
        "_G.__out.n == 2 and type(_G.__out.first) == 'table' and "
        "_G.__out.first.rows[1] == 1 and _G.__out.first.rows[2] == 2 and "
        "_G.__out.first.rows[3] == 3 and _G.__out.first.rows[4] == nil"));

    // Exactly-once: the session is consumed; a second completion loses.
    BOOST_CHECK_NE(api->lua_resume_session(nullptr, call.session, 1, R"([])"),
                   0);

    unref_parked(manager, runtime, svc.service_id, call);
    manager.exit(svc.service_id, "done");
}

// ---------------------------------------------------------------------------
// The main-thread refusal: lua_suspend_current on the VM's main state
// returns 0 so the plugin's Lua layer falls back to running inline.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SuspendCurrentRefusesMainThread) {
    auto* api = ensure_db_plugin_host();
    if (!api) {
        BOOST_TEST_MESSAGE("db-async fixture unavailable; skipping");
        return;
    }

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("dbasync_mod2.lua", "return {}\n");
    auto svc = manager.spawn(module, opts_for("dbasync_main_svc"));
    BOOST_REQUIRE(svc.success);

    std::atomic<bool> checked{false};
    std::atomic<bool> refused{false};
    manager.enqueue_forked_task(svc.service_id, [&] {
        auto vm = manager.service_vm(svc.service_id);
        if (!vm) return;
        lua_State* main_L = runtime.vm_main_state(vm);
        refused =
            api->lua_suspend_current(nullptr, main_L, 100, "db:test") == 0;
        checked = true;
    });
    BOOST_CHECK(wait_until([&] { return checked.load(); },
                           std::chrono::milliseconds(5000)));
    BOOST_CHECK(refused.load());
    manager.exit(svc.service_id, "done");
}

// ---------------------------------------------------------------------------
// Timeout: a parked coroutine with no worker completion is resumed as a
// failure with the stable {code="timeout"} payload; the session counts as
// consumed afterwards.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TimeoutCompletesSuspendedSession) {
    auto* api = ensure_db_plugin_host();
    if (!api) {
        BOOST_TEST_MESSAGE("db-async fixture unavailable; skipping");
        return;
    }

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("dbasync_mod3.lua", "return {}\n");
    auto svc = manager.spawn(module, opts_for("dbasync_timeout_svc"));
    BOOST_REQUIRE(svc.success);

    ParkedCall call;
    park_coroutine(manager, runtime, svc.service_id, call, 80, "db:test:query");
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_REQUIRE(call.parked);

    BOOST_CHECK(
        wait_until_lua(manager, runtime, svc.service_id,
                       "type(_G.__out) == 'table' and _G.__out.ok == false and "
                       "type(_G.__out.first) == 'table' and "
                       "_G.__out.first.code == 'timeout'"));

    // The timeout consumed the session: a late worker completion is
    // rejected — the connection-poison signal from the design doc.
    BOOST_CHECK_NE(
        api->lua_resume_session(nullptr, call.session, 1, R"([true])"), 0);

    unref_parked(manager, runtime, svc.service_id, call);
    manager.exit(svc.service_id, "done");
}

// ---------------------------------------------------------------------------
// Payload guards: malformed JSON and non-array payloads complete the
// session as a failure with db_async_bad_payload (exactly-one resume), and
// a NULL payload delivers a bare (true) with no values.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ResumePayloadGuards) {
    auto* api = ensure_db_plugin_host();
    if (!api) {
        BOOST_TEST_MESSAGE("db-async fixture unavailable; skipping");
        return;
    }

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("dbasync_mod4.lua", "return {}\n");
    auto svc = manager.spawn(module, opts_for("dbasync_payload_svc"));
    BOOST_REQUIRE(svc.success);

    auto park = [&](ParkedCall& call) {
        park_coroutine(manager, runtime, svc.service_id, call, 10000,
                       "db:test:query");
        BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                               std::chrono::milliseconds(5000)));
        BOOST_REQUIRE(call.parked);
    };

    // Malformed JSON: claim + failure completion.
    ParkedCall bad;
    park(bad);
    BOOST_CHECK(api->lua_resume_session(nullptr, bad.session, 1, "{not json") ==
                0);
    BOOST_CHECK(
        wait_until_lua(manager, runtime, svc.service_id,
                       "type(_G.__out) == 'table' and _G.__out.ok == false and "
                       "type(_G.__out.first) == 'table' and "
                       "_G.__out.first.code == 'db_async_bad_payload'"));
    BOOST_CHECK_NE(
        api->lua_resume_session(nullptr, bad.session, 1, R"([true])"), 0);
    unref_parked(manager, runtime, svc.service_id, bad);

    // Valid JSON but not an array: same guard, different message.
    ParkedCall nonarray;
    park(nonarray);
    BOOST_CHECK(api->lua_resume_session(nullptr, nonarray.session, 1,
                                        R"({"a":1})") == 0);
    BOOST_CHECK(
        wait_until_lua(manager, runtime, svc.service_id,
                       "type(_G.__out) == 'table' and _G.__out.ok == false and "
                       "type(_G.__out.first) == 'table' and "
                       "type(_G.__out.first.message) == 'string' and "
                       "_G.__out.first.message:find('not a JSON array', 1, "
                       "true) ~= nil"));
    unref_parked(manager, runtime, svc.service_id, nonarray);

    // NULL payload: bare (true) with no completion values.
    ParkedCall bare;
    park(bare);
    BOOST_CHECK(api->lua_resume_session(nullptr, bare.session, 1, nullptr) ==
                0);
    BOOST_CHECK(
        wait_until_lua(manager, runtime, svc.service_id,
                       "type(_G.__out) == 'table' and _G.__out.ok == true and "
                       "_G.__out.n == 1 and _G.__out.first == nil"));
    unref_parked(manager, runtime, svc.service_id, bare);

    manager.exit(svc.service_id, "done");
}

// ---------------------------------------------------------------------------
// Service exit while a session is pending: the worker's late completion
// must not touch the dead VM — the claim succeeds (the entry is consumed)
// and complete_call's caller-actor-gone arm drops it without a resume.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(WorkerResumeAfterServiceExitIsSafe) {
    auto* api = ensure_db_plugin_host();
    if (!api) {
        BOOST_TEST_MESSAGE("db-async fixture unavailable; skipping");
        return;
    }

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("dbasync_mod5.lua", "return {}\n");
    auto svc = manager.spawn(module, opts_for("dbasync_exit_svc"));
    BOOST_REQUIRE(svc.success);

    ParkedCall call;
    park_coroutine(manager, runtime, svc.service_id, call, 30000,
                   "db:test:query");
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_REQUIRE(call.parked);

    manager.exit(svc.service_id, "dbasync_exit");
    BOOST_CHECK(wait_until(
        [&] { return manager.query_service(svc.service_id).empty(); },
        std::chrono::milliseconds(5000)));

    // Late worker completion: consumed here (the claim wins), never routed —
    // the caller actor is gone and the entry is dropped, so the second
    // completion finds nothing.
    BOOST_CHECK(
        api->lua_resume_session(nullptr, call.session, 1, R"([true])") == 0);
    BOOST_CHECK_NE(
        api->lua_resume_session(nullptr, call.session, 1, R"([true])"), 0);
}
