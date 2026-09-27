// DB-async M3 end-to-end for the network drivers (docs/db-async-design.md):
// the REAL mysql / postgresql plugin .so files, started through a real
// PluginHost, serving live service actors.
//
// No database server is involved. A silent accept-only TCP peer makes the
// connect phase stall for a bounded, controllable time (mysql: the greeting
// read stalls until the driver's read/connect timeout; postgresql: libpq's
// connect_timeout bounds the whole attempt), which is exactly enough to
// exercise everything the M3 milestone adds around the driver:
//   - async submit suspends the caller; the ERROR round trip (the only
//     completable one without a server) rides the resume primitive back,
//     with the service responsive while parked (FIFO ordering assertion)
//   - acquire runs ON the worker (the M3 acceptance): with pool_size 2 and
//     three parked queries the third caller suspends too — the actor never
//     blocks. The pool_cv wait branch itself needs a real server holding
//     connections; what is proven here is the acquire-in-worker placement.
//   - caller timeout -> {code="timeout", retryable=true}; the late driver
//     failure is rejected, never overwriting the timeout result
//   - async: false -> the same call runs synchronously (first resume
//     completes; connect refused fails instantly)
//   - PluginHost::shutdown joins an in-flight worker mid-connect
//
// Each VM picks the namespaces up automatically: LuaRuntime::register_api
// runs global_host().register_lua_all on every service VM it creates. The
// shutdown case runs LAST — it stops the global plugin host.
#define BOOST_TEST_MODULE shield_db_net_async
#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <string>
#include <thread>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/plugin/plugin_host.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_db_net_async";

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
// Silent TCP peer: accepts every connection and never speaks. The drivers'
// connect attempts stall against it until their own timeouts fire — bounded,
// deterministic, no daemon. Held sockets are closed on stop.
// ---------------------------------------------------------------------------
class SilentServer {
public:
    SilentServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                          &len) == 0) {
            port_ = ntohs(addr.sin_port);
        }
        thread_ = std::thread([this] {
            while (running_.load()) {
                int c = ::accept(listen_fd_, nullptr, nullptr);
                if (c < 0) break;
                std::lock_guard lk(mu_);
                conns_.push_back(c);
            }
        });
    }
    ~SilentServer() { stop(); }

    uint16_t port() const { return port_; }
    bool ok() const { return listen_fd_ >= 0 && port_ != 0; }

    void stop() {
        if (!running_.exchange(false)) return;
        if (listen_fd_ >= 0) {
            // shutdown() wakes the blocking accept on Linux.
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
        std::lock_guard lk(mu_);
        for (int c : conns_) ::close(c);
        conns_.clear();
    }

private:
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{true};
    std::thread thread_;
    std::mutex mu_;
    std::vector<int> conns_;
};

// ---------------------------------------------------------------------------
// PluginHost startup with whichever driver packages this build produced.
// Per driver (binding prefix = "mysql." / "postgresql."):
//   <drv>.long  — 30s caller budget; connect stalls ~3-6s then fails
//   <drv>.short — 150ms caller budget; must time out before the driver gives
//                 up, and the late failure must NOT overwrite the timeout
//   <drv>.pool2 — pool_size 2; three parked queries overflow it
//   <drv>.off   — async disabled, pointed at a refused port (fails instantly,
//                 so the sync fallback case stays fast)
// ---------------------------------------------------------------------------
struct NetHost {
    SilentServer silent;
    const shield_host_api_v1* api = nullptr;
};

NetHost& ensure_net_host() {
    static NetHost host;
    static bool tried = false;
    if (tried) return host;
    tried = true;
    if (!host.silent.ok()) {
        BOOST_TEST_MESSAGE("silent TCP peer failed to start");
        return host;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(kTmpDir, ec);
    const fs::path plugins = kTmpDir + "/plugins";
    fs::remove_all(plugins, ec);
    fs::create_directories(plugins, ec);

    shield::plugin::PluginConfig pcfg;
    pcfg.directory = plugins.string();

    // The manifest ships the real config schema (async / call_timeout_ms
    // included), so the instance configs below are validated for real.
    auto lay_package = [&](const char* pkg, const fs::path& manifest_src,
                           const fs::path& so_src, const char* so_name) {
        const fs::path pkg_dir = plugins / pkg;
        fs::create_directories(pkg_dir / "bin", ec);
        fs::copy_file(manifest_src, pkg_dir / "manifest.yaml",
                      fs::copy_options::overwrite_existing, ec);
        fs::copy_file(so_src, pkg_dir / "bin" / so_name,
                      fs::copy_options::overwrite_existing, ec);
    };
    auto add_instances = [&](const char* pkg, const char* prefix) {
        auto add = [&](const char* suffix, nlohmann::json extra) {
            nlohmann::json config = {
                {"host", "127.0.0.1"},        {"port", host.silent.port()},
                {"database", "shield"},       {"username", "shield"},
                {"connect_timeout_ms", 6000}, {"query_timeout_ms", 3000},
                {"call_timeout_ms", 30000},
            };
            for (auto& kv : extra.items()) config[kv.key()] = kv.value();
            const std::string id = std::string(prefix) + "_" + suffix;
            shield::plugin::InstanceDecl decl;
            decl.id = id;
            decl.package = pkg;
            decl.required = true;
            decl.config = std::move(config);
            pcfg.instances.push_back(std::move(decl));
            shield::plugin::BindingDecl b;
            b.logical = std::string(prefix) + "." + suffix;
            b.instance_id = id;
            pcfg.bindings.push_back(std::move(b));
        };
        add("long", nlohmann::json::object());
        add("short", {{"call_timeout_ms", 150}});
        add("pool2", {{"pool_size", 2}});
        add("off", {{"async", false}, {"port", 1}});  // refused instantly
    };

#if defined(SHIELD_NET_MYSQL_LIBRARY)
    lay_package("database.mysql",
                std::string(SHIELD_SOURCE_DIR) + "/plugins/mysql/manifest.yaml",
                SHIELD_NET_MYSQL_LIBRARY, "libshield_db_mysql.so");
    add_instances("database.mysql", "mysql");
#endif
#if defined(SHIELD_NET_PGSQL_LIBRARY)
    lay_package(
        "database.postgresql",
        std::string(SHIELD_SOURCE_DIR) + "/plugins/postgresql/manifest.yaml",
        SHIELD_NET_PGSQL_LIBRARY, "libshield_db_pgsql.so");
    add_instances("database.postgresql", "postgresql");
#endif

    std::string error;
    if (!shield::plugin::global_host().startup(pcfg, error)) {
        BOOST_TEST_MESSAGE("net plugin startup failed: " << error);
        return host;
    }
    host.api = reinterpret_cast<const shield_host_api_v1*>(1);  // started
    return host;
}

// Evaluate `expr` on the service's main state until it returns Lua true.
// exec_lua stringifies tables, so every assertion is computed in Lua.
bool wait_until_lua(
    LuaServiceManager& manager, LuaRuntime& runtime,
    const std::string& service_id, const std::string& expr,
    std::chrono::milliseconds budget = std::chrono::milliseconds(20000)) {
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

// The body executed inside the parked coroutine; writes its outcome into
// _G.<slot> so several parks can be in flight at once. The chunk is loaded
// (not executed) and handed to the coroutine, so it takes (db, sql) as
// varargs and runs its statements directly — no factory wrapper.
std::string query_body(const std::string& slot) {
    return "local db, sql = ...\n"
           "_G." +
           slot +
           " = nil\n"
           "local ok, r = db:query(sql)\n"
           "if ok then\n"
           "    _G." +
           slot +
           " = {ok = true, n = #r, first = r[1]}\n"
           "else\n"
           "    _G." +
           slot +
           " = {ok = false, code = r and r.code or nil,\n"
           "       retryable = r and r.retryable or nil,\n"
           "       message = r and r.message or nil}\n"
           "end\n";
}

// The M4 transaction probe body: db:transaction over the __tx_* protocol.
// Against the silent peer __tx_begin never completes, so the body must never
// run — the outcome table distinguishes a begin failure from a body failure.
std::string tx_begin_body(const std::string& slot) {
    return "local db, sql = ...\n"
           "_G." +
           slot +
           " = nil\n"
           "_G." +
           slot +
           "_body_ran = nil\n"
           "local ok, err = db:transaction(function(tx)\n"
           "    _G." +
           slot +
           "_body_ran = true\n"
           "    return true\n"
           "end)\n"
           "if ok then\n"
           "    _G." +
           slot +
           " = {ok = true}\n"
           "else\n"
           "    _G." +
           slot +
           " = {ok = false, code = err and err.code or nil}\n"
           "end\n";
}

// Run `body` (a chunk taking (db, sql) as varargs) in a fresh coroutine on
// the service actor. Returns as soon as the coroutine has yielded into the
// shim (or fallen back to sync). binding, body and sql are copied by value:
// the task outlives this frame.
void park_body(LuaServiceManager& manager, LuaRuntime& runtime,
               const std::string& service_id, const char* ns,
               const std::string& binding, const std::string& body,
               const std::string& sql, ParkedCall& out) {
    const std::string sid = service_id;
    const std::string b = binding;
    const std::string q = sql;
    (void)manager.enqueue_forked_task(sid, [&, sid, ns, b, q, body] {
        g_park_error.clear();  // diagnostics report THIS park, not a stale one
        auto vm = manager.service_vm(sid);
        if (!vm) {
            g_park_error = "service_vm null";
            out.failed = true;
            return;
        }
        lua_State* main_L = runtime.vm_main_state(vm);
        const std::string setup =
            "local db = assert(" + std::string(ns) + "('" + b + "')) return db";
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

// Run `db:query(sql)` in a parked coroutine (the M3 probe body).
void park_query(LuaServiceManager& manager, LuaRuntime& runtime,
                const std::string& service_id, const char* ns,
                const std::string& binding, const std::string& sql,
                const std::string& slot, ParkedCall& out) {
    park_body(manager, runtime, service_id, ns, binding, query_body(slot), sql,
              out);
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

#if defined(SHIELD_NET_MYSQL_LIBRARY) || defined(SHIELD_NET_PGSQL_LIBRARY)

// ---------------------------------------------------------------------------
// The M3 acceptance, per driver: the connect stall parks the caller inside
// the shim, the actor keeps dispatching (side fork task runs before any
// completion can exist — FIFO order makes it deterministic), and the driver's
// eventual failure rides the resume path back with the documented error
// table shape.
// ---------------------------------------------------------------------------

void AsyncFailureRoundTripKeepsServiceResponsiveImpl(const char* ns,
                                                     const char* prefix) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx(std::string(prefix) + "_net_roundtrip");
    const std::string binding = std::string(prefix) + ".long";

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, ns, binding,
               "SELECT 1 AS v", "__db_out", call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());

    // While the connect stall is in flight the same service still dispatches
    // fork tasks. The side task and the completion resume are both actor
    // tasks in FIFO order, and the completion cannot even exist until the
    // worker finishes, so "side task ran before the result appeared" holds
    // no matter how fast the machine is.
    std::atomic<bool> side_ran{false};
    fx.manager.enqueue_forked_task(fx.service_id, [&] { side_ran = true; });

    // The driver gives up on the silent peer within its own timeouts.
    BOOST_REQUIRE(wait_until_lua(
        fx.manager, fx.runtime, fx.service_id,
        "type(_G.__db_out) == 'table' and _G.__db_out.ok == false and "
        "type(_G.__db_out.code) == 'string' and "
        "_G.__db_out.code ~= ''",
        std::chrono::milliseconds(20000)));
    BOOST_CHECK(side_ran.load());

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// Caller timeout (150ms) fires long before the driver's own give-up; the
// late failure completion must be rejected — the timeout result stands.
// ---------------------------------------------------------------------------

void AsyncCallerTimeoutImpl(const char* ns, const char* prefix) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx(std::string(prefix) + "_net_timeout");
    const std::string binding = std::string(prefix) + ".short";

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, ns, binding,
               "SELECT 1 AS v", "__db_out", call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_REQUIRE(call.parked.load());

    const bool got_timeout =
        wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                       "type(_G.__db_out) == 'table' and "
                       "_G.__db_out.ok == false and "
                       "_G.__db_out.code == 'timeout' and "
                       "_G.__db_out.retryable == true",
                       std::chrono::milliseconds(5000));
    if (!got_timeout) {
        // Distinguish "no resume ever delivered" from "wrong payload shape".
        std::string probe;
        lua_str(fx.manager, fx.runtime, fx.service_id,
                "type(_G.__db_out) ~= 'table' and tostring(_G.__db_out) or "
                "(function() local s = {} for k, v in pairs(_G.__db_out) do "
                "s[#s + 1] = tostring(k) .. '=' .. tostring(v) end return "
                "table.concat(s, ',') end)()",
                &probe);
        BOOST_TEST_MESSAGE("timeout probe __db_out: [" << probe << "]");
    }
    BOOST_REQUIRE(got_timeout);

    // Let the driver finish its (doomed) connect and attempt the late
    // completion; it must be dropped, not delivered over the timeout result.
    // 8s covers the slowest driver timeout in the fixture (6s).
    std::this_thread::sleep_for(std::chrono::milliseconds(8000));
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "type(_G.__db_out) == 'table' and "
                         "_G.__db_out.code == 'timeout'"));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// Pool exhaustion suspends instead of blocking the actor: pool_size 2, three
// parked queries. All three callers suspend; the actor keeps dispatching
// while every slot is stuck; all three eventually complete with errors.
// ---------------------------------------------------------------------------

void PoolExhaustionSuspendsCallersImpl(const char* ns, const char* prefix) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx(std::string(prefix) + "_net_pool");
    const std::string binding = std::string(prefix) + ".pool2";

    ParkedCall a, b, c;
    park_query(fx.manager, fx.runtime, fx.service_id, ns, binding,
               "SELECT 1 AS v", "__p1", a);
    BOOST_CHECK(wait_until([&] { return a.parked || a.failed; },
                           std::chrono::milliseconds(5000)));
    park_query(fx.manager, fx.runtime, fx.service_id, ns, binding,
               "SELECT 2 AS v", "__p2", b);
    BOOST_CHECK(wait_until([&] { return b.parked || b.failed; },
                           std::chrono::milliseconds(5000)));
    // Third caller: both pool slots are (about to be) held — its acquire
    // runs on a worker too; the actor never blocks.
    park_query(fx.manager, fx.runtime, fx.service_id, ns, binding,
               "SELECT 3 AS v", "__p3", c);
    BOOST_CHECK(wait_until([&] { return c.parked || c.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(a.parked.load());
    BOOST_REQUIRE(b.parked.load());
    BOOST_REQUIRE(c.parked.load());

    // The service is still dispatching while all three are suspended.
    std::atomic<bool> side_ran{false};
    fx.manager.enqueue_forked_task(fx.service_id, [&] { side_ran = true; });
    BOOST_CHECK(wait_until([&] { return side_ran.load(); },
                           std::chrono::milliseconds(5000)));

    // Every caller eventually gets its error round trip (each connect stall
    // is bounded by the driver timeouts; at most two run concurrently).
    BOOST_REQUIRE(
        wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                       "type(_G.__p1) == 'table' and _G.__p1.ok == false and "
                       "type(_G.__p1.code) == 'string' and "
                       "type(_G.__p2) == 'table' and _G.__p2.ok == false and "
                       "type(_G.__p2.code) == 'string' and "
                       "type(_G.__p3) == 'table' and _G.__p3.ok == false and "
                       "type(_G.__p3.code) == 'string'",
                       std::chrono::milliseconds(40000)));

    unref_parked(fx.manager, fx.runtime, fx.service_id, a);
    unref_parked(fx.manager, fx.runtime, fx.service_id, b);
    unref_parked(fx.manager, fx.runtime, fx.service_id, c);
}

// ---------------------------------------------------------------------------
// async: false — the same call inside a coroutine completes on the FIRST
// resume (no yield, no worker): connect to a refused port fails instantly on
// the actor thread, the documented sync semantics.
// ---------------------------------------------------------------------------

void AsyncFalseRunsSynchronouslyInsideCoroutineImpl(const char* ns,
                                                    const char* prefix) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx(std::string(prefix) + "_net_off");
    const std::string binding = std::string(prefix) + ".off";

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, ns, binding,
               "SELECT 7 AS v", "__db_out", call);
    BOOST_CHECK(
        wait_until([&] { return call.parked || call.failed || call.sync; },
                   std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    // Not parked: the coroutine ran to completion inside the first resume.
    BOOST_CHECK(call.sync.load());
    BOOST_CHECK(!call.parked.load());
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "type(_G.__db_out) == 'table' and "
                         "_G.__db_out.ok == false and "
                         "type(_G.__db_out.code) == 'string'"));
}

// ---------------------------------------------------------------------------
// M4: the async transaction's begin rides the worker path too. Against the
// silent peer __tx_begin's acquire+connect stalls, the caller parks inside
// the shim, the actor stays responsive, and the driver's eventual failure
// surfaces through db:transaction as (false, {code=...}) — with the body
// never having run (no token, no body execution).
// ---------------------------------------------------------------------------

void TxBeginFailureRoundTripImpl(const char* ns, const char* prefix) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx(std::string(prefix) + "_net_tx");
    const std::string binding = std::string(prefix) + ".long";

    ParkedCall call;
    park_body(fx.manager, fx.runtime, fx.service_id, ns, binding,
              tx_begin_body("__tx_out"), "SELECT 1 AS v", call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_TEST_MESSAGE("park error: " << g_park_error);
    BOOST_REQUIRE(call.parked.load());

    std::atomic<bool> side_ran{false};
    fx.manager.enqueue_forked_task(fx.service_id, [&] { side_ran = true; });

    BOOST_REQUIRE(wait_until_lua(
        fx.manager, fx.runtime, fx.service_id,
        "type(_G.__tx_out) == 'table' and _G.__tx_out.ok == false and "
        "type(_G.__tx_out.code) == 'string' and _G.__tx_out.code ~= ''",
        std::chrono::milliseconds(20000)));
    BOOST_CHECK(side_ran.load());
    // Begin failed before any token existed — the body never ran.
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "_G.__tx_out_body_ran == nil"));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// M4 gauges: while a tx begin is stalled on the connect the instance reports
// pending_async >= 1 and holding == 0 (nothing held — begin hasn't
// succeeded); after the failure drains, pending_async returns to 0.
// ---------------------------------------------------------------------------

void TxGaugesPendingAsyncWhileParkedImpl(const char* ns, const char* prefix) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx(std::string(prefix) + "_net_tx_gauge");
    const std::string binding = std::string(prefix) + ".long";
    const std::string instance = std::string(prefix) + "_long";

    ParkedCall call;
    park_body(fx.manager, fx.runtime, fx.service_id, ns, binding,
              tx_begin_body("__tx_out"), "SELECT 1 AS v", call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_REQUIRE(call.parked.load());

    BOOST_CHECK(wait_until(
        [&] {
            const auto* s = find_pool_stats(instance);
            return s && s->status == shield::plugin::PoolStatsStatus::ok &&
                   s->stats.pending_async >= 1 && s->stats.holding == 0;
        },
        std::chrono::milliseconds(5000)));

    BOOST_REQUIRE(wait_until_lua(
        fx.manager, fx.runtime, fx.service_id,
        "type(_G.__tx_out) == 'table' and _G.__tx_out.ok == false",
        std::chrono::milliseconds(20000)));
    BOOST_CHECK(wait_until(
        [&] {
            const auto* s = find_pool_stats(instance);
            return s && s->stats.pending_async == 0 && s->stats.holding == 0;
        },
        std::chrono::milliseconds(5000)));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

// ---------------------------------------------------------------------------
// M4 caller timeout on the tx begin: the 150ms budget expires while the
// connect still stalls; db:transaction surfaces the timeout error (the body
// never ran), and the late driver failure's completion is rejected — after
// the driver gives up, pending_async drains back to 0 with no held tx.
// ---------------------------------------------------------------------------

void TxCallerTimeoutRejectedBeginImpl(const char* ns, const char* prefix) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

    ServiceFixture fx(std::string(prefix) + "_net_tx_to");
    const std::string binding = std::string(prefix) + ".short";
    const std::string instance = std::string(prefix) + "_short";

    ParkedCall call;
    park_body(fx.manager, fx.runtime, fx.service_id, ns, binding,
              tx_begin_body("__tx_out"), "SELECT 1 AS v", call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_REQUIRE(call.parked.load());

    BOOST_REQUIRE(wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                                 "type(_G.__tx_out) == 'table' and "
                                 "_G.__tx_out.ok == false and "
                                 "_G.__tx_out.code == 'timeout'",
                                 std::chrono::milliseconds(5000)));
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "_G.__tx_out_body_ran == nil"));

    // 8s covers the slowest driver give-up in the fixture (6s); the rejected
    // begin completion needs no token cleanup, so the gauges just drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(8000));
    BOOST_CHECK(wait_until(
        [&] {
            const auto* s = find_pool_stats(instance);
            return s && s->stats.pending_async == 0 && s->stats.holding == 0;
        },
        std::chrono::milliseconds(5000)));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
}

#ifdef SHIELD_NET_MYSQL_LIBRARY
constexpr const char* kMysqlNs = "shield.database.mysql";
constexpr const char* kMysqlPrefix = "mysql";

BOOST_AUTO_TEST_CASE(MysqlAsyncFailureRoundTripKeepsServiceResponsive) {
    AsyncFailureRoundTripKeepsServiceResponsiveImpl(kMysqlNs, kMysqlPrefix);
}
BOOST_AUTO_TEST_CASE(MysqlAsyncCallerTimeout) {
    AsyncCallerTimeoutImpl(kMysqlNs, kMysqlPrefix);
}
BOOST_AUTO_TEST_CASE(MysqlPoolExhaustionSuspendsCallers) {
    PoolExhaustionSuspendsCallersImpl(kMysqlNs, kMysqlPrefix);
}
BOOST_AUTO_TEST_CASE(MysqlAsyncFalseRunsSynchronouslyInsideCoroutine) {
    AsyncFalseRunsSynchronouslyInsideCoroutineImpl(kMysqlNs, kMysqlPrefix);
}
BOOST_AUTO_TEST_CASE(MysqlTxBeginFailureRoundTrip) {
    TxBeginFailureRoundTripImpl(kMysqlNs, kMysqlPrefix);
}
BOOST_AUTO_TEST_CASE(MysqlTxGaugesPendingAsyncWhileParked) {
    TxGaugesPendingAsyncWhileParkedImpl(kMysqlNs, kMysqlPrefix);
}
BOOST_AUTO_TEST_CASE(MysqlTxCallerTimeoutRejectedBegin) {
    TxCallerTimeoutRejectedBeginImpl(kMysqlNs, kMysqlPrefix);
}
#endif  // SHIELD_NET_MYSQL_LIBRARY

#ifdef SHIELD_NET_PGSQL_LIBRARY
constexpr const char* kPgNs = "shield.database.postgresql";
constexpr const char* kPgPrefix = "postgresql";

BOOST_AUTO_TEST_CASE(PostgresqlAsyncFailureRoundTripKeepsServiceResponsive) {
    AsyncFailureRoundTripKeepsServiceResponsiveImpl(kPgNs, kPgPrefix);
}
BOOST_AUTO_TEST_CASE(PostgresqlAsyncCallerTimeout) {
    AsyncCallerTimeoutImpl(kPgNs, kPgPrefix);
}
BOOST_AUTO_TEST_CASE(PostgresqlPoolExhaustionSuspendsCallers) {
    PoolExhaustionSuspendsCallersImpl(kPgNs, kPgPrefix);
}
BOOST_AUTO_TEST_CASE(PostgresqlAsyncFalseRunsSynchronouslyInsideCoroutine) {
    AsyncFalseRunsSynchronouslyInsideCoroutineImpl(kPgNs, kPgPrefix);
}
BOOST_AUTO_TEST_CASE(PostgresqlTxBeginFailureRoundTrip) {
    TxBeginFailureRoundTripImpl(kPgNs, kPgPrefix);
}
BOOST_AUTO_TEST_CASE(PostgresqlTxGaugesPendingAsyncWhileParked) {
    TxGaugesPendingAsyncWhileParkedImpl(kPgNs, kPgPrefix);
}
BOOST_AUTO_TEST_CASE(PostgresqlTxCallerTimeoutRejectedBegin) {
    TxCallerTimeoutRejectedBeginImpl(kPgNs, kPgPrefix);
}
#endif  // SHIELD_NET_PGSQL_LIBRARY

// ---------------------------------------------------------------------------
// PluginHost::shutdown while a worker is mid-connect: the drain joins the
// worker (bounded by the driver timeout — no hang, no use-after-free) and
// the already-delivered timeout result is untouched by the late failure.
// Runs LAST: it stops the global plugin host.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ShutdownDrainsInFlightWorker) {
    if (!ensure_net_host().api) {
        BOOST_TEST_MESSAGE("net plugin fixture unavailable; skipping");
        return;
    }

#if defined(SHIELD_NET_MYSQL_LIBRARY)
    constexpr const char* ns = "shield.database.mysql";
    const std::string binding = "mysql.short";
#else
    constexpr const char* ns = "shield.database.postgresql";
    const std::string binding = "postgresql.short";
#endif

    ServiceFixture fx("net_shutdown");

    ParkedCall call;
    park_query(fx.manager, fx.runtime, fx.service_id, ns, binding,
               "SELECT 1 AS v", "__db_out", call);
    BOOST_CHECK(wait_until([&] { return call.parked || call.failed; },
                           std::chrono::milliseconds(5000)));
    BOOST_REQUIRE(call.parked.load());

    // Caller budget (150ms) expires while the connect is still stalled.
    BOOST_REQUIRE(wait_until_lua(fx.manager, fx.runtime, fx.service_id,
                                 "type(_G.__db_out) == 'table' and "
                                 "_G.__db_out.ok == false and "
                                 "_G.__db_out.code == 'timeout'",
                                 std::chrono::milliseconds(5000)));

    // Shutdown joins the worker mid-connect and stops every instance.
    const auto t0 = std::chrono::steady_clock::now();
    shield::plugin::global_host().shutdown();
    const auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
    BOOST_TEST_MESSAGE("shutdown drained in " << drain_ms << "ms");
    BOOST_CHECK_LT(drain_ms, 20000);  // bounded by driver timeouts, no hang

    // Late failure dropped: the timeout payload is still what the caller saw.
    // (No DB calls after shutdown — the instances are stopped.)
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    BOOST_CHECK(lua_bool(fx.manager, fx.runtime, fx.service_id,
                         "type(_G.__db_out) == 'table' and "
                         "_G.__db_out.code == 'timeout'"));

    unref_parked(fx.manager, fx.runtime, fx.service_id, call);
    ensure_net_host().silent.stop();
}

#endif  // SHIELD_NET_MYSQL_LIBRARY || SHIELD_NET_PGSQL_LIBRARY
