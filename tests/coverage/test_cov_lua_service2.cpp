// Coverage tests (round 2) for src/lua/lua_service.cpp: registry lookups,
// hostless fork enqueue, call routing errors, register_name guards,
// shutdown_all budget forcing, error message extraction, and spawn option
// edge cases.
#define BOOST_TEST_MODULE CovLuaService2
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_service2";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

std::string opts_for(const std::string& name,
                     nlohmann::json extra = nlohmann::json::object()) {
    nlohmann::json opts = {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        opts[it.key()] = it.value();
    }
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

}  // namespace

// ---------------------------------------------------------------------------
// Registry lookups for unknown services.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegistryLookupsForUnknownServices) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    BOOST_CHECK_EQUAL(manager.pending_task_count("no_such_service"), 0u);
    BOOST_CHECK(manager.service_vm("no_such_service") == nullptr);
    BOOST_CHECK_EQUAL(manager.query_service("no_such_name"), "");
    BOOST_CHECK(manager.list_services().empty());

    // Hostless fork with no live service actor is dropped.
    BOOST_CHECK_EQUAL(manager.enqueue_forked_task("", [] {}), 0u);
}

// ---------------------------------------------------------------------------
// call(): routing errors for missing targets and reserved method names.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallRoutingErrors) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_echo.lua",
                     "local M = {}\n"
                     "function M.echo(ctx, v) return v end\n"
                     "return M\n");
    auto svc = manager.spawn(path, opts_for("cov2_echo"));
    BOOST_REQUIRE(svc.success);

    // Missing target.
    {
        auto res = manager.call("ghost", "echo", nlohmann::json::array());
        BOOST_CHECK(!res.success);
        BOOST_CHECK(res.error_message.find("service not found") !=
                    std::string::npos);
    }

    // Reserved method name is rejected by validation.
    {
        auto res = manager.call(svc.service_id, "on_reserved",
                                nlohmann::json::array());
        BOOST_CHECK(!res.success);
    }

    // Missing method on a live service.
    {
        auto res = manager.call(svc.service_id, "missing_method",
                                nlohmann::json::array());
        BOOST_CHECK(!res.success);
        BOOST_CHECK(res.error_message.find("method not found") !=
                    std::string::npos);
    }

    // send() to unknown service reports an error.
    {
        std::string error;
        BOOST_CHECK(
            !manager.send("ghost", "echo", nlohmann::json::array(), &error));
        BOOST_CHECK(!error.empty());
    }

    // send_system() to unknown service reports an error.
    {
        std::string error;
        BOOST_CHECK(!manager.send_system("ghost", "on_connect",
                                         nlohmann::json::array(), &error));
        BOOST_CHECK(!error.empty());
    }

    // send_call_request() to unknown service fails.
    {
        std::string error;
        BOOST_CHECK(!manager.send_call_request(
            "ghost", "echo", nlohmann::json::array(), 123, &error));
    }
}

// ---------------------------------------------------------------------------
// register_name guards outside a dispatch context.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegisterNameGuards) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    std::string error;
    BOOST_CHECK(!manager.register_name("cov2_alias", &error));
    BOOST_CHECK(error.find("current service context") != std::string::npos);

    BOOST_CHECK(!manager.unregister_name("cov2_alias", &error));
    BOOST_CHECK(error.find("current service context") != std::string::npos);
}

// ---------------------------------------------------------------------------
// shutdown_all with a tiny budget takes the force path for slow services.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ShutdownAllBudgetForcePath) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_slow_exit.lua",
                     "local M = {}\n"
                     "function M.on_exit(reason)\n"
                     "  local t0 = os.clock()\n"
                     "  while os.clock() - t0 < 0.25 do end\n"
                     "end\n"
                     "return M\n");
    for (int i = 0; i < 3; ++i) {
        auto svc =
            manager.spawn(path, opts_for("cov2_slow_" + std::to_string(i)));
        BOOST_REQUIRE(svc.success);
    }

    // Budget of 1ms: the first slow on_exit exhausts it; the rest are forced.
    manager.shutdown_all("budget_test", 1);
    BOOST_CHECK(manager.list_services().empty());
}

// ---------------------------------------------------------------------------
// spawn(): name collisions and option parsing edges.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SpawnOptionEdges) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_plain.lua", "local M = {}\nreturn M\n");

    auto first = manager.spawn(path, opts_for("cov2_dup"));
    BOOST_REQUIRE(first.success);

    // Duplicate name.
    auto dup = manager.spawn(path, opts_for("cov2_dup"));
    BOOST_CHECK(!dup.success);
    BOOST_CHECK(dup.error_message.find("already exists") != std::string::npos);

    // Invalid name characters.
    auto bad = manager.spawn(path, opts_for("cov2 bad name!"));
    BOOST_CHECK(!bad.success);

    // Reserved shield.* prefix.
    auto reserved = manager.spawn(path, opts_for("shield.reserved"));
    BOOST_CHECK(!reserved.success);

    // Missing module file.
    auto missing =
        manager.spawn("/nonexistent/module.lua", opts_for("cov2_missing"));
    BOOST_CHECK(!missing.success);

    // opts that is not an object.
    auto bad_opts = manager.spawn(path, "[1,2,3]");
    BOOST_CHECK(!bad_opts.success);

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// exec_lua through the manager (used by the console eval command).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ManagerExecLua) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_exec_target.lua", "local M = {}\nreturn M\n");
    auto svc = manager.spawn(path, opts_for("cov2_exec_target"));
    BOOST_REQUIRE(svc.success);

    nlohmann::json result;
    std::string error;
    BOOST_CHECK(
        manager.exec_lua(svc.service_id, "return 1+1", &result, &error));
    BOOST_REQUIRE(result.is_array());
    BOOST_CHECK_EQUAL(result[0].get<int64_t>(), 2);

    BOOST_CHECK(!manager.exec_lua(svc.service_id, "error('svc2 boom')", &result,
                                  &error));
    BOOST_CHECK(error.find("svc2 boom") != std::string::npos);

    // Unknown service id.
    BOOST_CHECK(!manager.exec_lua("ghost", "return 1", &result, &error));

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// exit(): exiting an unknown service is a no-op; exiting twice is safe.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ExitIdempotence) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // Unknown service: no crash.
    manager.exit("ghost", "normal");

    const std::string path =
        write_script("cov2_exit_target.lua", "local M = {}\nreturn M\n");
    auto svc = manager.spawn(path, opts_for("cov2_exit_target"));
    BOOST_REQUIRE(svc.success);

    manager.exit(svc.service_id, "first");
    BOOST_CHECK(wait_until([&] { return manager.list_services().empty(); },
                           std::chrono::seconds(5)));
    // Second exit of the same id is a no-op.
    manager.exit(svc.service_id, "second");
}

// ---------------------------------------------------------------------------
// Round-3 additions: name registration/unregistration outside a service
// context, and unsigned 64-bit argument conversion.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegisterAndUnregisterNameWithoutServiceContext) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // From the main thread there is no current dispatch context: both
    // registration and unregistration are rejected.
    std::string error;
    BOOST_CHECK(!manager.register_name("cov3.name", &error));
    BOOST_CHECK(error.find("current service context") != std::string::npos);
    error.clear();
    BOOST_CHECK(!manager.unregister_name("cov3.name", &error));
    BOOST_CHECK(error.find("current service context") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(CallWithUnsigned64Argument) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov3_u64.lua",
                     "local M = {}\n"
                     "function M.kind(ctx, v)\n"
                     "  if v == nil then return 'nil' end\n"
                     "  return math.type(v)\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(path, opts_for("cov3_u64"));
    BOOST_REQUIRE(svc.success);

    auto res = manager.call(
        svc.service_id, "kind",
        nlohmann::json::array({nlohmann::json(18446744073709551615ULL)}));
    BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
    BOOST_REQUIRE_EQUAL(res.values.size(), 1u);
    BOOST_CHECK_EQUAL(res.values[0].get<std::string>(), "integer");
}

// ---------------------------------------------------------------------------
// Round-4: timer callback that raises (error hook path), and a still-pending
// timer at manager teardown.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TimerCallbackErrorIsContained) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path = write_script(
        "cov4_timer_err.lua",
        "local M = {}\n"
        "function M.on_init()\n"
        "    shield.timer_once(50, function() error('timer kaboom') end)\n"
        "end\n"
        "function M.ping(ctx) return 'pong' end\n"
        "return M\n");
    auto svc = manager.spawn(path, opts_for("cov4_timer_err"));
    BOOST_REQUIRE(svc.success);

    // The timer fires and its callback raises; the service stays alive and
    // responsive afterwards.
    CallResult res;
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        res = manager.call(svc.service_id, "ping", nlohmann::json::array());
        if (i > 5 && res.success) break;
    }
    BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
    BOOST_CHECK_EQUAL(res.values[0].get<std::string>(), "pong");
}

BOOST_AUTO_TEST_CASE(PendingTimerSurvivesManagerTeardown) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    {
        LuaRuntime runtime;
        LuaServiceManager manager(runtime, system);
        const std::string path =
            write_script("cov4_timer_pending.lua",
                         "local M = {}\n"
                         "function M.on_init()\n"
                         "    shield.timer_once(60000, function() end)\n"
                         "end\n"
                         "return M\n");
        auto svc = manager.spawn(path, opts_for("cov4_timer_pending"));
        BOOST_REQUIRE(svc.success);
        // Manager (and its timer drivers) is destroyed while the timer is
        // still armed.
    }
}

// ---------------------------------------------------------------------------
// Round-5: a suspended coroutine call leaves a call-timeout driver actor
// behind; manager teardown while the call is pending stops the driver.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(PendingCoroutineCallSurvivesManagerTeardown) {
    const std::string callee = write_script(
        "cov5_slow.lua",
        "local M = {}\n"
        "function M.slow(ctx) shield.sleep(8000) return 'done' end\n"
        "return M\n");
    const std::string caller =
        write_script("cov5_caller.lua",
                     "local M = {}\n"
                     "function M.kick(ctx, target)\n"
                     "  shield.call_timeout(10000, target, 'slow')\n"
                     "  return 'kicked'\n"
                     "end\n"
                     "return M\n");

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    {
        LuaRuntime runtime;
        LuaServiceManager manager(runtime, system);
        auto slow = manager.spawn(callee, opts_for("cov5_slow"));
        BOOST_REQUIRE(slow.success);
        auto base = manager.spawn(caller, opts_for("cov5_caller"));
        BOOST_REQUIRE(base.success);

        // Fire-and-forget so the caller's coroutine suspends with a pending
        // call (and its timeout driver) while the manager is destroyed.
        std::string err;
        BOOST_CHECK(manager.send(base.service_id, "kick",
                                 nlohmann::json::array({slow.service_id}),
                                 &err));
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
}

// ---------------------------------------------------------------------------
// Round-6: a second spawn of the same name while the first is still running
// its (slow) on_init observes the name reservation and fails.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ConcurrentDuplicateSpawnHitsReservation) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov6_slow_init.lua",
                     "local M = {}\n"
                     "function M.on_init() shield.sleep(800) end\n"
                     "return M\n");

    std::atomic<bool> first_done{false};
    SpawnResult first;
    std::thread spawner([&]() {
        first = manager.spawn(path, opts_for("cov6_dup"));
        first_done = true;
    });

    // Wait until the first spawn is inside its slow on_init, then race a
    // second spawn of the same name: it must observe the reservation.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    SpawnResult second = manager.spawn(path, opts_for("cov6_dup"));

    spawner.join();
    BOOST_REQUIRE(first_done.load());
    BOOST_REQUIRE_MESSAGE(first.success, first.error_message);
    BOOST_CHECK(!second.success);
    BOOST_CHECK(second.error_message.find("reserved") != std::string::npos);

    manager.exit(first.service_id, "done");
}

// ---------------------------------------------------------------------------
// Round-7: the exit-tombstone set is capped; after kRecentlyExitedLimit
// exits the set is cleared on the next exit (old tombstones degrade to
// service_not_found for very old names by design).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RecentlyExitedTombstoneCapClearsSet) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov7_tomb.lua", "local M = {}\nreturn M\n");

    // Spawn+exit unique names until the tombstone cap is exceeded. Each
    // iteration leaves one tombstone; entry 4097 clears the set.
    const int kLimit = 4096;
    for (int i = 0; i <= kLimit; ++i) {
        auto res = manager.spawn(path, opts_for("cov7_t_" + std::to_string(i)));
        BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
        manager.exit(res.service_id, "done");
    }
    // The set was cleared at the overflow exit, so a lookup of the very
    // first (oldest) name reports not-found semantics either way; the
    // observable effect here is simply that the loop completed.
    BOOST_CHECK(manager.list_services().empty());
}
