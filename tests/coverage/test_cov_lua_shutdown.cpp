// Coverage tests for the on_shutdown(ctx) drain phase
// (LuaServiceManager::drain_all, docs/lua-api.md on_shutdown contract).
//
// Exercises the manager-level surface bootstrap uses at shutdown: reverse
// spawn order, ctx shape, missing-hook no-op, hook-error continuation,
// shared-budget timeout (abandon + skip), spawn rejection while draining,
// and the inert zero-budget path. Lua fixtures are written to /tmp.
#define BOOST_TEST_MODULE CovLuaShutdown

#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_shutdown";

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

// Recorder service every scenario spawns FIRST, so its own on_shutdown runs
// LAST in the reverse-spawn drain (and first among the skipped when the
// budget runs out). Every other service's hook flushes its observation
// through shield.call("rec", "record", ...) — proving cross-service calls
// work during the drain phase.
std::string rec_script(bool with_hook) {
    std::string hook;
    if (with_hook) {
        hook =
            "function M.on_shutdown(ctx) table.insert(M.log, 'rec') "
            "return true end\n";
    }
    return "local M = {log = {}}\n"
           "function M.record(ctx, v) table.insert(M.log, tostring(v)) "
           "return true end\n"
           "function M.snapshot(ctx) return table.concat(M.log, ',') end\n" +
           hook + "return M\n";
}

// A service whose on_shutdown flushes its name plus its ctx through the
// recorder. The ctx assertions ride back as three extra return values:
// reason / timeout budget positive / deadline not behind the business clock.
std::string observing_script(const std::string& name) {
    return "local M = {}\n"
           "local st = {}\n"
           "function M.on_shutdown(ctx)\n"
           "  shield.call('rec', 'record', '" +
           name +
           "')\n"
           "  st.reason = tostring(ctx.reason)\n"
           "  st.timeout_ok = (ctx.timeout_ms ~= nil and ctx.timeout_ms > 0)\n"
           "  st.deadline_ok = (ctx.deadline_ms ~= nil and "
           "ctx.deadline_ms >= shield.now())\n"
           "  return true\n"
           "end\n"
           "function M.ctx_report(ctx)\n"
           "  return st.reason, st.timeout_ok, st.deadline_ok\n"
           "end\n"
           "return M\n";
}

std::string snapshot(LuaServiceManager& manager) {
    CallResult r = manager.call("rec", "snapshot", nlohmann::json::array());
    BOOST_REQUIRE(r.success);
    BOOST_REQUIRE(!r.values.empty());
    return r.values[0].get<std::string>();
}

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

}  // namespace

// Hook errors and timeouts must not stop the drain: services queued behind
// the failing one still get their turn, in reverse spawn order.
BOOST_AUTO_TEST_CASE(DrainRunsInReverseSpawnOrder) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string rec = write_script("sd_rec.lua", rec_script(true));
    const std::string mid = write_script("sd_mid.lua", observing_script("mid"));
    const std::string last =
        write_script("sd_last.lua", observing_script("last"));

    BOOST_REQUIRE(manager.spawn(rec, opts_for("rec")).success);
    BOOST_REQUIRE(manager.spawn(mid, opts_for("sd_mid")).success);
    BOOST_REQUIRE(manager.spawn(last, opts_for("sd_last")).success);

    manager.drain_all("stopping", 5000);

    // Spawn order rec, mid, last -> drain visits last, mid, rec. The ctx
    // observations of every hook ran before the recorder's own hook.
    BOOST_CHECK_EQUAL(snapshot(manager), "last,mid,rec");

    CallResult mid_ctx =
        manager.call("sd_mid", "ctx_report", nlohmann::json::array());
    BOOST_REQUIRE(mid_ctx.success);
    BOOST_CHECK_EQUAL(mid_ctx.values[0].get<std::string>(), "stopping");
    BOOST_CHECK(mid_ctx.values[1].get<bool>());  // timeout_ms > 0
    BOOST_CHECK(mid_ctx.values[2].get<bool>());  // deadline_ms >= shield.now()

    manager.shutdown_all("stopping", 2000);
}

// A service without on_shutdown is an immediate no-op success: the drain
// adds no latency, nothing is recorded, and the runtime stays functional.
BOOST_AUTO_TEST_CASE(MissingHookIsNoop) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string rec = write_script("sd_rec_noop.lua", rec_script(false));
    const std::string plain = write_script("sd_plain.lua", "return {}\n");

    BOOST_REQUIRE(manager.spawn(rec, opts_for("rec")).success);
    BOOST_REQUIRE(manager.spawn(plain, opts_for("sd_plain")).success);

    manager.drain_all("stopping", 5000);

    // Neither service implements on_shutdown: nothing ran, nothing failed.
    BOOST_CHECK_EQUAL(snapshot(manager), "");

    manager.shutdown_all("stopping", 2000);
}

// A hook that raises must be logged and skipped, not fatal: services behind
// it still drain, in order.
BOOST_AUTO_TEST_CASE(HookErrorContinuesDrain) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string rec = write_script("sd_rec_err.lua", rec_script(true));
    // Records "ok" through the recorder.
    const std::string ok_svc =
        write_script("sd_ok.lua", observing_script("ok"));
    // Records "bad-start" through the recorder, then raises.
    const std::string bad_svc =
        write_script("sd_bad.lua",
                     "local M = {}\n"
                     "function M.on_shutdown(ctx)\n"
                     "  shield.call('rec', 'record', 'bad-start')\n"
                     "  error('boom on_shutdown')\n"
                     "end\n"
                     "return M\n");

    BOOST_REQUIRE(manager.spawn(rec, opts_for("rec")).success);
    BOOST_REQUIRE(manager.spawn(ok_svc, opts_for("sd_ok")).success);
    BOOST_REQUIRE(manager.spawn(bad_svc, opts_for("sd_bad")).success);

    manager.drain_all("stopping", 5000);

    // Reverse order visits sd_bad (errors after recording), sd_ok, rec: the
    // error did not stop the drain and the order still holds.
    BOOST_CHECK_EQUAL(snapshot(manager), "bad-start,ok,rec");

    manager.shutdown_all("stopping", 2000);
}

// The drain budget is shared across the phase: a hook parked past it is
// abandoned (WARN), the remaining services are skipped, and the whole phase
// still returns within the budget. shutdown_all afterwards tears the parked
// service down.
BOOST_AUTO_TEST_CASE(DrainTimeoutAbandonsAndSkips) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string rec = write_script("sd_rec_to.lua", rec_script(true));
    const std::string stuck = write_script("sd_stuck.lua",
                                           "local M = {}\n"
                                           "function M.on_shutdown(ctx)\n"
                                           "  shield.sleep(10000)\n"
                                           "end\n"
                                           "return M\n");
    const std::string quick =
        write_script("sd_quick.lua", observing_script("quick"));

    // Spawn rec, stuck, quick -> reverse order visits quick, stuck, rec:
    // quick drains, stuck eats the rest of the shared budget, rec is
    // skipped by the exhausted-deadline break.
    BOOST_REQUIRE(manager.spawn(rec, opts_for("rec")).success);
    BOOST_REQUIRE(manager.spawn(stuck, opts_for("sd_stuck")).success);
    BOOST_REQUIRE(manager.spawn(quick, opts_for("sd_quick")).success);

    const auto start = std::chrono::steady_clock::now();
    manager.drain_all("stopping", 500);
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    BOOST_CHECK_EQUAL(snapshot(manager), "quick");
    // Bounded by the 500ms budget; the slack covers scheduler jitter on the
    // Debug tree, not the parked hook's 10s sleep.
    BOOST_CHECK_LT(wall_ms, 3000);

    manager.shutdown_all("stopping", 2000);
}

// shield.spawn from inside a drain hook is rejected: the contract forbids
// spawning during the drain, and the hook observes the failure like any
// spawn failure (and keeps draining afterwards).
BOOST_AUTO_TEST_CASE(SpawnRejectedDuringDrain) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string rec = write_script("sd_rec_spawn.lua", rec_script(true));
    const std::string child_path = write_script("sd_child.lua", "return {}\n");
    // The module path is baked into the script: the hook spawns a service
    // that must be rejected because the runtime is draining.
    const std::string spawner =
        write_script("sd_spawner.lua",
                     "local M = {}\n"
                     "local st = {}\n"
                     "function M.on_shutdown(ctx)\n"
                     "  local h, err = shield.spawn('" +
                         child_path +
                         "', {name = 'sd_late_child'})\n"
                         "  st.spawn_ok = (h ~= nil)\n"
                         "  st.spawn_err = tostring(err or '')\n"
                         "  shield.call('rec', 'record', 'spawn-attempted')\n"
                         "end\n"
                         "function M.spawn_report(ctx)\n"
                         "  return st.spawn_ok, st.spawn_err\n"
                         "end\n"
                         "return M\n");

    BOOST_REQUIRE(manager.spawn(rec, opts_for("rec")).success);
    BOOST_REQUIRE(manager.spawn(spawner, opts_for("sd_spawner")).success);

    manager.drain_all("stopping", 5000);

    // The drain continued past the rejected spawn (recorder got the record)
    // and the hook observed the failure.
    BOOST_CHECK_EQUAL(snapshot(manager), "spawn-attempted,rec");
    CallResult r =
        manager.call("sd_spawner", "spawn_report", nlohmann::json::array());
    BOOST_REQUIRE(r.success);
    BOOST_CHECK(!r.values[0].get<bool>());  // spawn returned nil
    BOOST_CHECK(!r.values[1].get<std::string>().empty());

    // The rejected spawn left nothing behind.
    const auto names = manager.list_services();
    BOOST_CHECK(std::find(names.begin(), names.end(), "sd_late_child") ==
                names.end());

    // The guard stays latched after the drain phase: a direct manager.spawn
    // is rejected with the drain-specific reason too.
    auto late = manager.spawn(child_path, opts_for("sd_too_late"));
    BOOST_CHECK(!late.success);
    BOOST_CHECK(late.error_message.find("draining") != std::string::npos);

    manager.shutdown_all("stopping", 2000);
}

// drain_budget_ms <= 0 keeps the whole phase inert: no hook runs (this is
// the documented "feature off" default).
BOOST_AUTO_TEST_CASE(ZeroBudgetIsInert) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string rec = write_script("sd_rec_zero.lua", rec_script(true));
    BOOST_REQUIRE(manager.spawn(rec, opts_for("rec")).success);

    manager.drain_all("stopping", 0);

    BOOST_CHECK_EQUAL(snapshot(manager), "");

    manager.shutdown_all("stopping", 2000);
}
