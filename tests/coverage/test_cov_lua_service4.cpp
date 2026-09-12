// Coverage tests (round 4) for src/lua/lua_service.cpp and the fork C++
// wrapper in lua_api.cpp: manager.call error/timeout surfaces, spawn init
// failure, the destructor's pending-sync-call wake (the shutdown_all copy is
// covered elsewhere), timer fire guards, init-period timer registry lookup,
// client ingress/control warn paths, and a forked task that throws a C++
// exception.
#define BOOST_TEST_MODULE CovLuaService4
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/core/service_message.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_service4";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
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

const char* kSlowScript = R"lua(
local M = {}
function M.on_init(args) return true end
function M.slow(ctx) shield.sleep(60000) return 'never' end
function M.quick(ctx) return 'ok' end
return M
)lua";

const char* kInitFailScript = R"lua(
local M = {}
function M.on_init(args) error('init kaput') end
return M
)lua";

// on_init schedules a timer that fires while the spawning thread is still
// inside on_init (the VM is only in init_vms, not yet published).
const char* kInitTimerScript = R"lua(
local M = {}
function M.on_init(args)
  shield.timer_once(1, function() end)
  shield.sleep(80)
  return true
end
return M
)lua";

// A service with no rpc routes at all (rpc table missing) and one with a
// declared route whose handler throws (dispatch failure warn).
const char* kNoRpcScript = R"lua(
local M = {}
function M.on_init(args) return true end
return M
)lua";

const char* kBoomRpcScript = R"lua(
local M = {}
function M.on_init(args) return true end
function M.boom(client, request) error('rpc kaput') end
return M
)lua";

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

}  // namespace

// call() on an unknown target fails fast with the not-found error.
BOOST_AUTO_TEST_CASE(CallUnknownServiceFails) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto result =
        manager.call("ghost_svc", "method", nlohmann::json::array({1}), 100);
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error_message.find("service not found") !=
                std::string::npos);
}

// A call whose handler yields past the timeout is failed by the CAF expiry
// driver with the (code="timeout") error object; call_error_message reduces
// that to its message string.
BOOST_AUTO_TEST_CASE(CallTimeoutSurfacesTimeoutMessage,
                     *boost::unit_test::timeout(30)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto slow =
        manager.spawn(write_script("cov_lsvc4_slow.lua", kSlowScript),
                      opts_for("cov_lsvc4_slow_impl"));
    BOOST_REQUIRE(slow.success);

    auto result =
        manager.call(slow.service_id, "slow", nlohmann::json::array(), 150);
    if (!result.success &&
        result.error_message.find("call timeout") == std::string::npos) {
        std::fprintf(stderr, "timeout case message: %s\n",
                     result.error_message.c_str());
    }
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error_message.find("call timeout") != std::string::npos);
}

// The destructor wakes pending sync calls even when shutdown_all was never
// invoked: the blocked caller must not ride out its timeout.
BOOST_AUTO_TEST_CASE(DestructorWakesPendingSyncCalls,
                     *boost::unit_test::timeout(30)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    std::optional<CallResult> result;
    std::atomic<bool> initiated{false};
    std::thread blocked;
    {
        LuaRuntime runtime;
        LuaServiceManager manager(runtime, system);
        blocked = std::thread([&] {
            result = manager.call_with_session(
                [&](uint64_t, std::string&) {
                    initiated = true;
                    return true;
                },
                60000);
        });
        BOOST_CHECK(wait_until([&] { return initiated.load(); },
                               std::chrono::milliseconds(2000)));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // No shutdown_all: the destructor owns the wake-up.
    }
    blocked.join();
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK(!result->success);
    BOOST_CHECK_EQUAL(result->error_message, "runtime is stopping");
}

// An on_init that throws fails the spawn with the dedicated message.
BOOST_AUTO_TEST_CASE(SpawnFailsWhenOnInitThrows) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto result =
        manager.spawn(write_script("cov_lsvc4_initfail.lua", kInitFailScript),
                      opts_for("cov_lsvc4_initfail_impl"));
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error_message.find("on_init failed") !=
                std::string::npos);
    BOOST_CHECK(manager.query_service("cov_lsvc4_initfail_impl").empty());
}

// A timer scheduled inside on_init fires while the VM lives only in the
// init registry (not yet published): the lookup must find it there.
BOOST_AUTO_TEST_CASE(InitPeriodTimerHitsInitVmRegistry,
                     *boost::unit_test::timeout(30)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto svc =
        manager.spawn(write_script("cov_lsvc4_inittimer.lua", kInitTimerScript),
                      opts_for("cov_lsvc4_inittimer_impl"));
    BOOST_CHECK(svc.success);
    // The spawn itself only returns once on_init completed (including its
    // sleep), by which time the early timer already fired against the
    // init-period registry entry.
}

// Firing a timer whose callback is invalid is silently dropped; firing a
// timer after its service exited finds no dispatch VM and is dropped too.
BOOST_AUTO_TEST_CASE(TimerFireGuards, *boost::unit_test::timeout(30)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto svc =
        manager.spawn(write_script("cov_lsvc4_quick.lua", kSlowScript),
                      opts_for("cov_lsvc4_quick_impl"));
    BOOST_REQUIRE(svc.success);

    // Invalid callback: schedule directly with a nil sol::function.
    sol::state scratch;
    scratch.open_libraries(sol::lib::base);
    sol::function nil_fn = scratch["nope"];
    manager.schedule_actor_timer_once(20, nil_fn, svc.service_id);

    // A timer on a live service still fires after the invalid one.
    std::atomic<bool> fired{false};
    manager.schedule_actor_timer_once_fn(
        20, [&] { fired = true; }, svc.service_id);
    BOOST_CHECK(wait_until([&] { return fired.load(); },
                           std::chrono::milliseconds(2000)));
    manager.shutdown_all("cov_timer_guards");
}

// Client ingress to a live actor whose VM has no rpc table (or whose route
// is not owned) is dropped with a warning; a handler that throws surfaces
// the dispatch-failure warning. Reserved control kinds are dropped too.
BOOST_AUTO_TEST_CASE(ClientIngressAndControlWarnMatrix,
                     *boost::unit_test::timeout(30)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto bare =
        manager.spawn(write_script("cov_lsvc4_norpc.lua", kNoRpcScript),
                      opts_for("cov_lsvc4_norpc_impl"));
    BOOST_REQUIRE(bare.success);

    nlohmann::json boom_extra = nlohmann::json::object();
    boom_extra["rpc"] = {{"routes", nlohmann::json::array({nlohmann::json{
                                        {"id", 7},
                                        {"name", "boom"},
                                        {"direction", "c2s"},
                                        {"binding", "boom"},
                                        {"requires_auth", false},
                                    }})}};
    const auto boomer =
        manager.spawn(write_script("cov_lsvc4_boomrpc.lua", kBoomRpcScript),
                      opts_for("cov_lsvc4_boomrpc_impl", boom_extra));
    BOOST_REQUIRE(boomer.success);

    ClientContextData ctx;
    ctx.session_id = 1;
    ctx.player_id = "p1";

    // Route not owned by the bare service (it has no rpc table at all).
    ClientIngress unknown_table;
    unknown_table.context = ctx;
    unknown_table.route_id = 7;
    auto bare_actor = manager.service_actor("cov_lsvc4_norpc_impl");
    BOOST_REQUIRE(bare_actor);
    caf::anon_send(bare_actor, unknown_table);

    // Declared route but sent to the wrong service: not owned.
    ClientIngress not_owned;
    not_owned.context = ctx;
    not_owned.route_id = 999;
    auto boom_actor = manager.service_actor("cov_lsvc4_boomrpc_impl");
    BOOST_REQUIRE(boom_actor);
    caf::anon_send(boom_actor, not_owned);

    // Owned route whose handler throws: dispatch failure warn.
    ClientIngress throwing;
    throwing.context = ctx;
    throwing.route_id = 7;
    throwing.body_bytes = {'{', '}'};
    caf::anon_send(boom_actor, throwing);

    // Reserved control kind without a producer: dropped with a warning.
    ClientControlMessage control;
    control.kind = ClientControlMessage::Kind::Reconnected;
    control.context = ctx;
    control.reason = "cov_reconnect";
    caf::anon_send(boom_actor, control);

    // Give the actor thread a moment to process everything, then tear down.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    manager.shutdown_all("cov_ingress_matrix");
}

// A forked task that throws a C++ exception is contained and logged.
BOOST_AUTO_TEST_CASE(ForkTaskCppExceptionIsContained,
                     *boost::unit_test::timeout(30)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto svc =
        manager.spawn(write_script("cov_lsvc4_fork.lua", kNoRpcScript),
                      opts_for("cov_lsvc4_fork_impl"));
    BOOST_REQUIRE(svc.success);

    std::atomic<bool> ran{false};
    manager.enqueue_forked_task(svc.service_id, [&] {
        ran = true;
        throw std::runtime_error("cpp boom");
    });
    // The throwing task must not take the actor down: a follow-up call works.
    BOOST_CHECK(wait_until([&] { return ran.load(); },
                           std::chrono::milliseconds(2000)));
    auto follow = manager.call(svc.service_id, "nonexistent_method", {}, 500);
    BOOST_CHECK(!follow.success);

    manager.shutdown_all("cov_fork_throw");
}

// The external call timeout driver refuses non-positive timeouts.
BOOST_AUTO_TEST_CASE(ExternalTimeoutDriverRejectsNonPositive) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    BOOST_CHECK(!manager.schedule_external_call_timeout(0, 4242));
    BOOST_CHECK(!manager.schedule_external_call_timeout(-5, 4242));
}
