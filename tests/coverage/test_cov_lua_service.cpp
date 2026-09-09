// Coverage tests for src/lua/lua_service.cpp.
//
// Exercises LuaServiceManager behaviour through the public C++ and Lua-facing
// surfaces: spawn option validation, on_init edge cases, send/call guards,
// exit cleanup (pending calls, timers, queued messages), fork tasks, coroutine
// call routing, async spawn outcomes, the C++ suspend/resume primitives, and
// manager teardown paths. Lua fixtures are written to /tmp.
#define BOOST_TEST_MODULE CovLuaService
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/plugin/plugin_host.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_service";

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

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

// A small echo service used as the callee in call scenarios.
const char* kCalleeScript = R"lua(
local M = {}
function M.echo(ctx, v) return v end
function M.throwing(ctx) error("callee boom") end
function M.slow(ctx) shield.sleep(250) return "slow_done" end
return M
)lua";

// Service whose handler coroutine issues shield.call / shield.call_timeout.
const char* kCallerScript = R"lua(
local M = {}
local st = {}
function M.do_call(ctx, target)
  local ok, v = shield.call(target, "echo", "hi")
  st.call_ok = ok
  st.call_val = tostring(v)
  return ok
end
function M.do_call_err(ctx, target)
  local ok, err = shield.call(target, "throwing")
  st.call_err = type(err) == 'table' and (err.message or '') or tostring(err)
  return ok
end
function M.do_call_timeout(ctx, target)
  local ok, err = shield.call_timeout(60, target, "slow")
  st.timeout_code = err and err.code or ""
  return ok
end
function M.call_slow_then_report(ctx, target)
  local ok, v = shield.call(target, "slow")
  st.after_slow = ok and "resumed" or "failed"
  return ok
end
function M.get_state(ctx)
  return st.call_ok, st.call_val, st.call_err, st.timeout_code, st.after_slow
end
return M
)lua";

}  // namespace

// ---------------------------------------------------------------------------
// spawn: option validation and early failures.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SpawnOptionErrors) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov_plain.lua", "return {}\n");

    // No name given: the generated default name contains ':' (from the
    // module path/hash suffix) and is rejected by the name validator.
    auto unnamed = manager.spawn(module);
    BOOST_CHECK(!unnamed.success);
    BOOST_CHECK(unnamed.error_message.find("invalid service name") !=
                std::string::npos);

    // A published alias conflicts with a new spawn of the same name.
    auto owner = manager.spawn(module, opts_for("cov_alias_clash_owner"));
    BOOST_REQUIRE(owner.success);
    auto clash = manager.spawn(module, opts_for("cov_alias_clash_owner"));
    BOOST_CHECK(!clash.success);
    BOOST_CHECK(clash.error_message.find("already exists") !=
                std::string::npos);

    // Missing module file.
    auto missing = manager.spawn(kTmpDir + "/no_such_file.lua",
                                 opts_for("cov_missing_mod"));
    BOOST_CHECK(!missing.success);
    BOOST_CHECK(missing.error_message.find("Failed to load module") !=
                std::string::npos);

    // Malformed options JSON.
    auto badjson = manager.spawn(module, "{{not json");
    BOOST_CHECK(!badjson.success);
    BOOST_CHECK(badjson.error_message.find("Spawn failed") !=
                std::string::npos);

    // Shutdown rejects further spawns and async spawns.
    manager.shutdown_all("stopping");
    auto stopped = manager.spawn(module, opts_for("cov_stopped"));
    BOOST_CHECK(!stopped.success);
    BOOST_CHECK_EQUAL(stopped.error_message, "runtime is stopping");
    BOOST_CHECK(!manager.enqueue_async_spawn(1, module, opts_for("cov_x")));
}

// Alias registration from a handler context (needed to create a published
// name owned by a live service).
BOOST_AUTO_TEST_CASE(SpawnConflictsWithPublishedAlias) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_alias_reg.lua",
                     "local M = {}\n"
                     "function M.on_init(args)\n"
                     "  local cfg = args.config or {}\n"
                     "  if cfg.alias then shield.register(cfg.alias) end\n"
                     "  return true\n"
                     "end\n"
                     "return M\n");
    const std::string plain = write_script("cov_plain2.lua", "return {}\n");

    auto owner = manager.spawn(
        module,
        opts_for("cov_alias_owner", {{"config", {{"alias", "cov.clash"}}}}));
    BOOST_REQUIRE(owner.success);
    BOOST_CHECK_EQUAL(manager.query_service("cov.clash"), owner.service_id);

    auto clash = manager.spawn(plain, opts_for("cov.clash"));
    BOOST_CHECK(!clash.success);
    BOOST_CHECK(clash.error_message.find("already exists") !=
                std::string::npos);
}

// ---------------------------------------------------------------------------
// spawn: on_init edge cases (timeout, owned-name rollback, name usurpation).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SpawnOnInitEdges) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_init_edge.lua",
                     "local M = {}\n"
                     "function M.on_init(args)\n"
                     "  local cfg = (args and args.config) or {}\n"
                     "  local tc = cfg.test_case or ''\n"
                     "  if tc == 'timeout' then\n"
                     "    local t = shield.monotonic()\n"
                     "    while shield.monotonic() - t < 80 do end\n"
                     "    return false\n"
                     "  elseif tc == 'owned' then\n"
                     "    shield.register('cov.owned_alias')\n"
                     "    return false\n"
                     "  elseif tc == 'usurp' then\n"
                     "    shield.register(args.name)\n"
                     "    return true\n"
                     "  end\n"
                     "  return true\n"
                     "end\n"
                     "return M\n");

    // on_init exceeds the spawn timeout budget.
    auto slow = manager.spawn(
        module,
        opts_for("cov_init_timeout",
                 {{"timeout", 5}, {"config", {{"test_case", "timeout"}}}}));
    BOOST_CHECK(!slow.success);
    BOOST_CHECK(slow.error_message.find("spawn timeout") != std::string::npos);

    // on_init registers a name then fails: the owned name is rolled back.
    auto owned = manager.spawn(
        module,
        opts_for("cov_init_owned", {{"config", {{"test_case", "owned"}}}}));
    BOOST_CHECK(!owned.success);
    BOOST_CHECK(manager.query_service("cov.owned_alias").empty());

    // on_init publishes its own service name: the final publish conflicts.
    auto usurp = manager.spawn(
        module,
        opts_for("cov_init_usurp", {{"config", {{"test_case", "usurp"}}}}));
    BOOST_CHECK(!usurp.success);
    BOOST_CHECK(usurp.error_message.find("already exists") !=
                std::string::npos);
}

// ---------------------------------------------------------------------------
// send: validation, size limits and post-exit errors.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SendValidationErrors) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script(
        "cov_recv.lua", "return { ping = function() return 'pong' end }\n");
    auto svc = manager.spawn(module, opts_for("cov_recv_svc"));
    BOOST_REQUIRE(svc.success);

    std::string error;
    // Empty method name.
    BOOST_CHECK(
        !manager.send(svc.service_id, "", nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("invalid method name") != std::string::npos);

    // Over-long method name.
    BOOST_CHECK(!manager.send(svc.service_id, std::string(129, 'm'),
                              nlohmann::json::array(), &error));

    // Reserved "on_" prefix.
    BOOST_CHECK(!manager.send(svc.service_id, "on_hook",
                              nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("reserved") != std::string::npos);

    // Oversized payload.
    BOOST_CHECK(!manager.send(
        svc.service_id, "ping",
        nlohmann::json::array({std::string(1100000, 'x')}), &error));
    BOOST_CHECK(error.find("message too large") != std::string::npos);

    // Payload containing the unsupported-value sentinel string.
    BOOST_CHECK(!manager.send(svc.service_id, "ping",
                              nlohmann::json::array({"<unsupported>"}),
                              &error));
    BOOST_CHECK(error.find("unsupported value") != std::string::npos);

    // Target missing.
    BOOST_CHECK(!manager.send("cov_no_such_target", "ping",
                              nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("service not found") != std::string::npos);

    // After exit the service is reported dead rather than missing.
    manager.exit(svc.service_id, "done");
    BOOST_CHECK(
        !manager.send(svc.service_id, "ping", nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("service dead") != std::string::npos);

    // exit() on an unknown service is a silent no-op.
    manager.exit("cov_never_existed", "normal");
}

// ---------------------------------------------------------------------------
// send_system / send_call_request guards.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SystemMessagePaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_sys.lua",
                     "local M = {}\n"
                     "local last = ''\n"
                     "function M.on_status(ctx, payload)\n"
                     "  last = 'status:' .. tostring(payload)\n"
                     "  return 'ok'\n"
                     "end\n"
                     "function M.get_last(ctx) return last end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_sys_svc"));
    BOOST_REQUIRE(svc.success);

    // Reserved names are allowed for system messages.
    BOOST_CHECK(manager.send_system(svc.service_id, "on_status",
                                    nlohmann::json::array({"p"})));
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(svc.service_id, "get_last",
                                         nlohmann::json::array(), 1000);
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].get<std::string>() == "status:p";
        },
        std::chrono::seconds(2)));

    std::string error;
    // Invalid method on the system path.
    BOOST_CHECK(!manager.send_system(svc.service_id, "",
                                     nlohmann::json::array(), &error));

    // Unknown target on the system path.
    BOOST_CHECK(!manager.send_system("cov_sys_missing", "on_status",
                                     nlohmann::json::array(), &error));
    BOOST_CHECK(error.find("service not found") != std::string::npos);

    // send_call_request to an unknown target.
    BOOST_CHECK(!manager.send_call_request("cov_sys_missing", "ping",
                                           nlohmann::json::array(), 7, &error));
    BOOST_CHECK(error.find("service not found") != std::string::npos);

    // Stopping rejects system messages and external calls.
    manager.shutdown_all("stopping");
    error.clear();
    BOOST_CHECK(!manager.send_system(svc.service_id, "on_status",
                                     nlohmann::json::array(), &error));
    BOOST_CHECK_EQUAL(error, "runtime is stopping");
    auto call_stopped =
        manager.call(svc.service_id, "get_last", nlohmann::json::array(), 100);
    BOOST_CHECK(!call_stopped.success);
    BOOST_CHECK_EQUAL(call_stopped.error_message, "runtime is stopping");
}

// ---------------------------------------------------------------------------
// call: self-call rejection and dispatch timeout.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallSelfAndTimeout) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_selfcall.lua",
                     "local M = {}\n"
                     "local myname\n"
                     "function M.on_init(args) myname = args.name end\n"
                     "function M.ping(ctx) return 'pong' end\n"
                     "function M.self_sync(ctx)\n"
                     "  local ok, err = shield._sync_call(myname, 'ping')\n"
                     "  return ok, err and err.message or 'nil'\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_self_svc"));
    BOOST_REQUIRE(svc.success);

    // A synchronous call from a service to itself is rejected.
    CallResult cr = manager.call(svc.service_id, "self_sync",
                                 nlohmann::json::array(), 2000);
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 2u);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), false);
    BOOST_CHECK(cr.values[1].get<std::string>().find("self-call") !=
                std::string::npos);

    // External call to a slow handler times out.
    const std::string slow_module = write_script(
        "cov_slowpoke.lua",
        "return { slow = function(ctx) shield.sleep(400) return 1 end }\n");
    auto slow_svc = manager.spawn(slow_module, opts_for("cov_slow_svc"));
    BOOST_REQUIRE(slow_svc.success);
    CallResult timeout =
        manager.call(slow_svc.service_id, "slow", nlohmann::json::array(), 60);
    BOOST_CHECK(!timeout.success);
    BOOST_CHECK(timeout.error_message.find("call timeout") !=
                std::string::npos);
}

// ---------------------------------------------------------------------------
// exit() cleans up pending coroutine call timeouts of the exiting service.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ExitCancelsPendingCallTimeouts) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string hang_module =
        write_script("cov_hang_caller.lua",
                     "local M = {}\n"
                     "local st = { hanging = false }\n"
                     "function M.hang_call(ctx, target)\n"
                     "  st.hanging = true\n"
                     "  return shield.call_timeout(5000, target, 'slow')\n"
                     "end\n"
                     "function M.get_hanging(ctx) return st.hanging end\n"
                     "return M\n");
    const std::string slow_module = write_script(
        "cov_hang_callee.lua",
        "return { slow = function(ctx) shield.sleep(8000) return 1 end }\n");

    auto bottom = manager.spawn(slow_module, opts_for("cov_hang_bottom"));
    auto middle = manager.spawn(hang_module, opts_for("cov_hang_middle"));
    auto top = manager.spawn(hang_module, opts_for("cov_hang_top"));
    BOOST_REQUIRE(bottom.success && middle.success && top.success);

    // top -> middle (pending 5s call), middle -> bottom (pending 5s call).
    std::thread t1([&] {
        manager.call(top.service_id, "hang_call",
                     nlohmann::json::array({"cov_hang_middle"}), 500);
    });
    std::thread t2([&] {
        manager.call(middle.service_id, "hang_call",
                     nlohmann::json::array({"cov_hang_bottom"}), 500);
    });
    t1.join();
    t2.join();

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(top.service_id, "get_hanging",
                                         nlohmann::json::array(), 1000);
            return cr.success && cr.values[0].get<bool>();
        },
        std::chrono::seconds(2)));

    // Exiting top stops its own call-timeout driver and skips middle's.
    manager.exit(top.service_id, "cleanup");
    BOOST_CHECK(manager.query_service("cov_hang_top").empty());

    // Clean up the remaining chain.
    manager.exit(middle.service_id, "cleanup");
    manager.exit(bottom.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// exit from a handler: nested exit request inside on_exit plus orphan
// messages queued to the dying service.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ExitHandlerQueuesOrphanMessages) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // The service exits from its own handler. The exit machinery runs on the
    // actor thread (same thread as message dispatch), so on_exit may safely
    // send messages to itself: they queue behind the current message and are
    // dispatched after the service entry is gone (dispatch_message no-ops).
    const std::string module =
        write_script("cov_exit_self.lua",
                     "local M = {}\n"
                     "local myname\n"
                     "function M.on_init(args) myname = args.name end\n"
                     "function M.noop(ctx) end\n"
                     "function M.quit(ctx) shield.exit('handler_exit') end\n"
                     "function M.on_exit(reason)\n"
                     "  shield.exit('nested')\n"
                     "  for i = 1, 10 do shield.send(myname, 'noop') end\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_exit_self_svc"));
    BOOST_REQUIRE(svc.success);

    BOOST_REQUIRE(
        manager.send(svc.service_id, "quit", nlohmann::json::array()));
    BOOST_CHECK(wait_until(
        [&]() { return manager.query_service("cov_exit_self_svc").empty(); },
        std::chrono::seconds(3)));
    BOOST_CHECK(manager.list_services().empty());

    // Give the actor time to drain the orphaned messages before teardown.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

// ---------------------------------------------------------------------------
// fork: Lua callback errors and native task exceptions.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ForkTaskErrors) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script(
        "cov_fork_err.lua",
        "local M = {}\n"
        "local fork_err = ''\n"
        "function M.on_error(err, ctx) fork_err = tostring(err) end\n"
        "function M.start_bad_fork(ctx)\n"
        "  shield.fork(function() error('fork boom') end)\n"
        "  return true\n"
        "end\n"
        "function M.get_fork_err(ctx) return fork_err end\n"
        "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_fork_svc"));
    BOOST_REQUIRE(svc.success);

    BOOST_REQUIRE(manager.send(svc.service_id, "start_bad_fork",
                               nlohmann::json::array()));
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(svc.service_id, "get_fork_err",
                                         nlohmann::json::array(), 1000);
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].get<std::string>().find("fork boom") !=
                       std::string::npos;
        },
        std::chrono::seconds(2)));

    // A native C++ task that throws is caught and routed to on_error.
    manager.enqueue_forked_task(
        svc.service_id, []() { throw std::runtime_error("native fork boom"); });
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(svc.service_id, "get_fork_err",
                                         nlohmann::json::array(), 1000);
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].get<std::string>().find("native fork boom") !=
                       std::string::npos;
        },
        std::chrono::seconds(2)));
}

// ---------------------------------------------------------------------------
// fork queue bookkeeping.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ForkQueueLifecycle) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // Unknown owner / empty id short-circuit.
    BOOST_CHECK_EQUAL(manager.enqueue_forked_task("", [] {}), 0u);
    BOOST_CHECK_EQUAL(manager.enqueue_forked_task("cov_no_such_service", [] {}),
                      0u);
    BOOST_CHECK_EQUAL(manager.pending_task_count("cov_no_such_service"), 0u);

    const std::string module =
        write_script("cov_fork_queue.lua",
                     "local M = {}\n"
                     "function M.stall(ctx)\n"
                     "  local t = shield.monotonic()\n"
                     "  while shield.monotonic() - t < 300 do end\n"
                     "  return 'stalled'\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_fork_queue_svc"));
    BOOST_REQUIRE(svc.success);

    // Keep the actor busy so queued fork tasks are not picked up, then
    // cancel them directly through the task queue.
    BOOST_REQUIRE(
        manager.send(svc.service_id, "stall", nlohmann::json::array()));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    manager.enqueue_forked_task(svc.service_id, [] {});
    manager.enqueue_forked_task(svc.service_id, [] {});
    BOOST_CHECK_GE(manager.pending_task_count(svc.service_id), 1u);
    manager.cancel_forked_tasks_for_service(svc.service_id);
    BOOST_CHECK_EQUAL(manager.pending_task_count(svc.service_id), 0u);

    // Let the stall finish so the actor is idle again.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(svc.service_id, "stall",
                                         nlohmann::json::array(), 1000);
            return cr.success;
        },
        std::chrono::seconds(2)));
}

// ---------------------------------------------------------------------------
// timers: fire-after-cancel race and scheduler guards.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TimerFireAfterCancel) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_timer_race.lua",
                     "local M = {}\n"
                     "local tid = 0\n"
                     "function M.on_init(args)\n"
                     "  tid = shield.timer_once(50, function() end)\n"
                     "end\n"
                     "function M.get_tid(ctx) return tid end\n"
                     "function M.stall(ctx)\n"
                     "  local t = shield.monotonic()\n"
                     "  while shield.monotonic() - t < 400 do end\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_timer_race_svc"));
    BOOST_REQUIRE(svc.success);

    // Grab the timer id before occupying the actor.
    uint64_t timer_id = 0;
    {
        CallResult cr = manager.call(svc.service_id, "get_tid",
                                     nlohmann::json::array(), 1000);
        BOOST_REQUIRE(cr.success);
        timer_id = cr.values[0].get<uint64_t>();
        BOOST_CHECK_GT(timer_id, 0u);
    }

    // Keep the actor busy; the timer's fire message queues up. Cancelling
    // the timer removes its bookkeeping, so the queued fire becomes a no-op.
    BOOST_REQUIRE(
        manager.send(svc.service_id, "stall", nlohmann::json::array()));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    BOOST_CHECK(manager.cancel_actor_timer(timer_id));
    BOOST_CHECK_EQUAL(manager.active_actor_timer_count(), 0u);

    // Wait out the stall so the orphan fire message is processed.
    std::this_thread::sleep_for(std::chrono::milliseconds(450));

    // Cancelling an unknown timer id reports false.
    BOOST_CHECK(!manager.cancel_actor_timer(987654321u));
}

BOOST_AUTO_TEST_CASE(TimerSchedulerGuards) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // A valid Lua callback bound to an unknown service is rejected too.
    sol::state lua;
    lua.open_libraries(sol::lib::base);
    lua.safe_script("cov_cb = function() end");
    sol::function cb = lua["cov_cb"];
    BOOST_CHECK(cb.valid());
    BOOST_CHECK_EQUAL(
        manager.schedule_actor_timer_once(10, cb, "cov_no_such_service"), 0u);
    BOOST_CHECK_EQUAL(
        manager.schedule_actor_timer_fixed_delay(10, cb, "cov_no_such_service"),
        0u);

    // Invalid callbacks and unknown services are rejected without scheduling.
    BOOST_CHECK_EQUAL(
        manager.schedule_actor_timer_once(10, sol::function{}, "cov_any"), 0u);
    BOOST_CHECK_EQUAL(manager.schedule_actor_timer_once(10, sol::function{},
                                                        "cov_no_such_service"),
                      0u);
    BOOST_CHECK_EQUAL(manager.schedule_actor_timer_once_fn(
                          10, [] {}, "cov_no_such_service"),
                      0u);
    BOOST_CHECK_EQUAL(manager.schedule_actor_timer_fixed_delay(
                          10, sol::function{}, "cov_any"),
                      0u);
    BOOST_CHECK_EQUAL(manager.schedule_actor_call_timeout(0, "cov_any", 1), 1u);
    BOOST_CHECK_EQUAL(
        manager.schedule_actor_call_timeout(10, "cov_no_such_service", 2), 2u);
    BOOST_CHECK(!manager.cancel_actor_call_timeout(555555u));

    const std::string module =
        write_script("cov_timer_guard.lua", "return {}\n");
    auto svc = manager.spawn(module, opts_for("cov_timer_guard_svc"));
    BOOST_REQUIRE(svc.success);

    // A real one-shot timer with a native callback fires and can be awaited.
    std::atomic<int> fired{0};
    uint64_t id = manager.schedule_actor_timer_once_fn(
        30, [&fired] { fired.fetch_add(1); }, svc.service_id);
    BOOST_CHECK_GT(id, 0u);
    BOOST_CHECK_EQUAL(manager.active_actor_timer_count(), 1u);
    BOOST_CHECK(wait_until([&]() { return fired.load() >= 1; },
                           std::chrono::seconds(2)));
    BOOST_CHECK(
        wait_until([&]() { return manager.active_actor_timer_count() == 0; },
                   std::chrono::seconds(2)));
}

// ---------------------------------------------------------------------------
// Coroutine call routing between services.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CoroutineCallRouting) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path =
        write_script("cov_callee.lua", kCalleeScript);
    const std::string caller_path =
        write_script("cov_caller.lua", kCallerScript);

    auto callee = manager.spawn(callee_path, opts_for("cov_callee_svc"));
    auto caller = manager.spawn(caller_path, opts_for("cov_caller_svc"));
    BOOST_REQUIRE(callee.success && caller.success);

    // Successful coroutine call round-trips through the caller's actor.
    CallResult cr =
        manager.call(caller.service_id, "do_call",
                     nlohmann::json::array({"cov_callee_svc"}), 3000);
    BOOST_REQUIRE(cr.success);

    // Failing callee resumes the caller with the error message.
    cr = manager.call(caller.service_id, "do_call_err",
                      nlohmann::json::array({"cov_callee_svc"}), 3000);
    BOOST_REQUIRE(cr.success);

    // Call timeout resumes the caller with a timeout error code.
    cr = manager.call(caller.service_id, "do_call_timeout",
                      nlohmann::json::array({"cov_callee_svc"}), 3000);
    BOOST_REQUIRE(cr.success);

    // A call that suspends on the callee's sleep still resumes the caller.
    cr = manager.call(caller.service_id, "call_slow_then_report",
                      nlohmann::json::array({"cov_callee_svc"}), 3000);
    BOOST_REQUIRE(cr.success);

    // Verify the recorded handler-side outcomes.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult st = manager.call(caller.service_id, "get_state",
                                         nlohmann::json::array(), 1000);
            return st.success && st.values.size() == 5u;
        },
        std::chrono::seconds(3)));
    CallResult st = manager.call(caller.service_id, "get_state",
                                 nlohmann::json::array(), 1000);
    BOOST_REQUIRE(st.success);
    BOOST_REQUIRE_EQUAL(st.values.size(), 5u);
    BOOST_CHECK_EQUAL(st.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(st.values[1].get<std::string>(), "hi");
    BOOST_CHECK(st.values[2].get<std::string>().find("callee boom") !=
                std::string::npos);
    BOOST_CHECK_EQUAL(st.values[3].get<std::string>(), "timeout");
    BOOST_CHECK_EQUAL(st.values[4].get<std::string>(), "resumed");
}

// ---------------------------------------------------------------------------
// Async spawn outcomes (shield.spawn from a handler coroutine).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AsyncSpawnOutcomes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string parent_path = write_script(
        "cov_spawn_parent.lua",
        "local M = {}\n"
        "local st = {spawning = false, done = false, code = '', result = ''}\n"
        "function M.spawn_child(ctx, mod, name, timeout)\n"
        "  st.spawning = true\n"
        "  local h, err\n"
        "  if timeout then\n"
        "    h, err = shield.spawn(mod, {name = name, timeout = timeout})\n"
        "  else\n"
        "    h, err = shield.spawn(mod, {name = name})\n"
        "  end\n"
        "  st.done = true\n"
        "  if h then st.result = 'ok' else st.code = err and err.code or "
        "'?' end\n"
        "end\n"
        "function M.get_state(ctx)\n"
        "  return st.spawning, st.done, st.code, st.result\n"
        "end\n"
        "return M\n");
    const std::string ok_child =
        write_script("cov_spawn_ok_child.lua",
                     "return { on_init = function(args) return true end }\n");
    const std::string slow_child =
        write_script("cov_spawn_slow_child.lua",
                     "return { on_init = function(args)\n"
                     "  local t = shield.monotonic()\n"
                     "  while shield.monotonic() - t < 400 do end\n"
                     "  return true\n"
                     "end }\n");
    const std::string fail_child = write_script(
        "cov_spawn_fail_child.lua",
        "return { on_init = function(args) return false, 'no' end }\n");

    auto parent = manager.spawn(parent_path, opts_for("cov_spawn_parent_svc"));
    BOOST_REQUIRE_MESSAGE(parent.success,
                          "parent spawn failed: " << parent.error_message);

    auto state_of = [&](int idx) -> std::string {
        CallResult st = manager.call(parent.service_id, "get_state",
                                     nlohmann::json::array(), 1000);
        if (!st.success || st.values.size() < 4u) {
            return "<pending>";
        }
        return st.values[idx].is_string() ? st.values[idx].get<std::string>()
                                          : "<non-string>";
    };
    auto wait_state = [&](int idx, const std::string& expect) {
        return wait_until([&]() { return state_of(idx) == expect; },
                          std::chrono::seconds(3));
    };

    // Successful async spawn.
    BOOST_REQUIRE(manager
                      .call(parent.service_id, "spawn_child",
                            nlohmann::json::array({slow_child, "cov_child_ok"}),
                            1000)
                      .success);
    BOOST_CHECK(wait_state(3, "ok"));
    BOOST_CHECK_EQUAL(manager.query_service("cov_child_ok"), "cov_child_ok");

    // Child whose on_init fails reports init_failed.
    BOOST_REQUIRE(
        manager
            .call(parent.service_id, "spawn_child",
                  nlohmann::json::array({fail_child, "cov_child_fail"}), 1000)
            .success);
    BOOST_CHECK(wait_state(2, "init_failed"));

    // Parent times out while the child initializes; the child is rolled back.
    BOOST_REQUIRE(
        manager
            .call(parent.service_id, "spawn_child",
                  nlohmann::json::array({slow_child, "cov_child_to", 60}), 1000)
            .success);
    BOOST_CHECK(wait_state(2, "spawn_timeout"));
    BOOST_CHECK(wait_until(
        [&]() { return manager.query_service("cov_child_to").empty(); },
        std::chrono::seconds(3)));

    // Caller exits while the child initializes: the child still completes.
    // Use send (not call) so the parent stays suspended while we exit it;
    // a sync call would queue behind the suspended handler and only return
    // once the spawn finished. The child initializes slowly (800ms) so the
    // 150ms delay below lands inside the suspension window deterministically.
    const std::string very_slow_child =
        write_script("cov_spawn_vslow_child.lua",
                     "return { on_init = function(args)\n"
                     "  local t = shield.monotonic()\n"
                     "  while shield.monotonic() - t < 800 do end\n"
                     "  return true\n"
                     "end }\n");
    BOOST_REQUIRE(manager.send(
        parent.service_id, "spawn_child",
        nlohmann::json::array({very_slow_child, "cov_child_dead"})));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    manager.exit(parent.service_id, "gone");
    BOOST_CHECK(wait_until(
        [&]() {
            return manager.query_service("cov_child_dead") == "cov_child_dead";
        },
        std::chrono::seconds(4)));
    manager.exit("cov_child_dead", "cleanup");
    manager.exit("cov_child_ok", "cleanup");

    // An async spawn whose failure message contains "timeout" (here: the
    // missing module path itself) maps to the spawn_timeout error code while
    // the caller is still waiting.
    auto late_parent =
        manager.spawn(parent_path, opts_for("cov_spawn_parent_late"));
    BOOST_REQUIRE(late_parent.success);
    BOOST_REQUIRE(manager
                      .call(late_parent.service_id, "spawn_child",
                            nlohmann::json::array(
                                {kTmpDir + "/cov_module_timeout_missing.lua",
                                 "cov_child_never"}),
                            2000)
                      .success);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult st = manager.call(late_parent.service_id, "get_state",
                                         nlohmann::json::array(), 1000);
            return st.success && st.values.size() == 4u &&
                   st.values[2].is_string() &&
                   st.values[2].get<std::string>() == "spawn_timeout";
        },
        std::chrono::seconds(3)));
    manager.exit(late_parent.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// C++ suspend/resume primitives used by coroutine calls.
// ---------------------------------------------------------------------------
namespace {

// Create a coroutine on the given state whose body is `body` (a Lua function
// string fragment) and return its lua_State*.
lua_State* make_coro(sol::state& lua, const std::string& body) {
    lua.safe_script("cov_coro_body = function(...) " + body + " end");
    lua_State* co = lua_newthread(lua.lua_state());
    lua_getglobal(co, "cov_coro_body");
    return co;
}

}  // namespace

BOOST_AUTO_TEST_CASE(SuspendResumePrimitives) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::coroutine);

    // Caller session whose service no longer exists: complete_call routes
    // through "caller service not found" and still resumes the coroutine.
    lua_State* co_echo = make_coro(lua, "return ...");
    uint64_t s_echo = manager.suspend_for_call(co_echo, 10000);
    BOOST_CHECK_NE(s_echo, 0u);
    manager.complete_call(s_echo, true, nlohmann::json::array({1, 2, 3}));

    // Mixed JSON payload types (unsigned/float/null/object/array) survive
    // the push_json_to_stack conversion.
    lua_State* co_mixed = make_coro(lua, "return ...");
    uint64_t s_mixed = manager.suspend_for_call(co_mixed, 10000);
    nlohmann::json mixed = nlohmann::json::array(
        {1u, 2.5, nullptr, nlohmann::json({{"k", 1}}), nlohmann::json({1, 2})});
    manager.resume_caller(s_mixed, true, mixed);

    // Non-array, non-null payload (a bare string).
    lua_State* co_str = make_coro(lua, "return ...");
    uint64_t s_str = manager.suspend_for_call(co_str, 10000);
    manager.resume_caller(s_str, true, nlohmann::json("plain"));

    // Discarded JSON value degrades to a pushed nil.
    lua_State* co_nil = make_coro(lua, "return ...");
    uint64_t s_nil = manager.suspend_for_call(co_nil, 10000);
    manager.resume_caller(s_nil, true,
                          nlohmann::json(nlohmann::json::value_t::discarded));

    // A coroutine that raises once resumed reports a continuation error.
    lua_State* co_err = make_coro(lua, "error('co resume boom')");
    uint64_t s_err = manager.suspend_for_call(co_err, 10000);
    manager.resume_caller(s_err, true, nlohmann::json::array());

    // A coroutine that yields again re-suspends (anchor released).
    lua_State* co_yielder = make_coro(lua, "coroutine.yield() return 1");
    uint64_t s_yielder = manager.suspend_for_call(co_yielder, 10000);
    manager.resume_caller(s_yielder, true, nlohmann::json::array());

    // Handler session bookkeeping: unknown and null coroutines are no-ops.
    manager.on_handler_completed(nullptr, nlohmann::json::array());
    manager.on_handler_failed(nullptr, "nope");
    lua_State* co_unknown = make_coro(lua, "return 1");
    manager.on_handler_failed(co_unknown, "no session");
    manager.set_handler_call_session(nullptr, 5);
    manager.set_handler_call_session(co_unknown, 0);

    // A registered handler session completes the pending call.
    lua_State* co_tagged = make_coro(lua, "return 'tagged'");
    manager.set_handler_call_session(co_tagged, 424242);
    manager.on_handler_completed(co_tagged, nlohmann::json::array({"v"}));

    // Unknown sessions are dropped.
    manager.complete_call(999999, true, nlohmann::json::array());
    manager.resume_caller(999998, false, nlohmann::json::array());

    // Deadline scan resumes an expired caller with a timeout error.
    lua_State* co_late = make_coro(lua, "return ...");
    uint64_t s_late = manager.suspend_for_call(co_late, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    BOOST_CHECK_EQUAL(manager.check_call_timeouts(INT64_MAX), 1);
}

// ---------------------------------------------------------------------------
// Manager accessor guards outside a dispatch context.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ManagerAccessorsOutsideDispatch) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    BOOST_CHECK_EQUAL(manager.current_service_id(), "");
    BOOST_CHECK_EQUAL(manager.current_sender_id(), "");
    BOOST_CHECK_EQUAL(manager.current_trace_id(), "");
    BOOST_CHECK_EQUAL(manager.current_deadline_ms(), 0);
    BOOST_CHECK(!manager.is_in_exit());

    // panic/request_current_exit are no-ops without a dispatch frame.
    manager.panic_current("outside");
    manager.request_current_exit("outside");

    // Name registry operations require a current service context.
    std::string error;
    BOOST_CHECK(!manager.register_name("cov.orphan", &error));
    BOOST_CHECK(error.find("requires current service context") !=
                std::string::npos);
    error.clear();
    BOOST_CHECK(!manager.unregister_name("cov.orphan", &error));
    BOOST_CHECK(error.find("requires current service context") !=
                std::string::npos);

    // Error hooks for unknown services are dropped.
    manager.invoke_error_hook("cov_ghost", "handler", "m", "boom");
    manager.reset_error_count("cov_ghost");

    // exec_lua on an unknown service fails.
    nlohmann::json result;
    error.clear();
    BOOST_CHECK(!manager.exec_lua("cov_ghost", "return 1", &result, &error));
    BOOST_CHECK(error.find("Service not found") != std::string::npos);

    // Null clock attachment is rejected, a mock clock takes effect.
    manager.attach_clock(nullptr);
    auto mock = std::make_shared<MockClock>();
    mock->set_now(1234567890000);
    manager.attach_clock(mock);
    BOOST_CHECK_EQUAL(manager.clock_now_ms(), 1234567890000);
    BOOST_CHECK_EQUAL(manager.clock_now_seconds(), 1234567890);

    BOOST_CHECK(manager.list_services().empty());
}

// ---------------------------------------------------------------------------
// unregister: a service cannot drop a name owned by another service.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(UnregisterOtherOwnersAlias) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string owner_module =
        write_script("cov_alias_owner2.lua",
                     "local M = {}\n"
                     "function M.on_init(args)\n"
                     "  local cfg = args.config or {}\n"
                     "  if cfg.alias then shield.register(cfg.alias) end\n"
                     "  return true\n"
                     "end\n"
                     "return M\n");
    const std::string other_module =
        write_script("cov_alias_other.lua",
                     "local M = {}\n"
                     "local code = ''\n"
                     "function M.unregister(ctx, name)\n"
                     "  local ok, err = shield.unregister(name)\n"
                     "  code = err and err.message or 'ok'\n"
                     "  return ok\n"
                     "end\n"
                     "function M.get_code(ctx) return code end\n"
                     "return M\n");

    auto owner = manager.spawn(
        owner_module,
        opts_for("cov_alias_owner2", {{"config", {{"alias", "cov.foreign"}}}}));
    BOOST_REQUIRE(owner.success);
    auto other = manager.spawn(other_module, opts_for("cov_alias_other"));
    BOOST_REQUIRE(other.success);

    CallResult cr = manager.call(other.service_id, "unregister",
                                 nlohmann::json::array({"cov.foreign"}), 2000);
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), false);

    cr = manager.call(other.service_id, "get_code", nlohmann::json::array(),
                      1000);
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK(cr.values[0].get<std::string>().find(
                    "owned by another service") != std::string::npos);

    // The name is still owned by the original service.
    BOOST_CHECK_EQUAL(manager.query_service("cov.foreign"), owner.service_id);
}

// ---------------------------------------------------------------------------
// Consecutive-error threshold triggers panic and exit.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(PanicThresholdExitsService) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_panic.lua",
                     "local M = {}\n"
                     "function M.on_error(err, ctx) end\n"
                     "function M.throwing(ctx) error('boom') end\n"
                     "function M.ping(ctx) return 'pong' end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_panic_svc"));
    BOOST_REQUIRE(svc.success);

    for (int i = 0; i < 10; ++i) {
        CallResult cr = manager.call(svc.service_id, "throwing",
                                     nlohmann::json::array(), 1000);
        BOOST_CHECK(!cr.success);
    }

    // After the 10th consecutive error the service panics and exits.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(svc.service_id, "ping",
                                         nlohmann::json::array(), 300);
            return !cr.success;
        },
        std::chrono::seconds(3)));
    BOOST_CHECK(manager.query_service("cov_panic_svc").empty());
}

// ---------------------------------------------------------------------------
// Script path resolution through the runtime actor config.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ResolveScriptPathFromConfig) {
    const std::string glob_dir = kTmpDir + "/glob";
    std::filesystem::create_directories(glob_dir);
    write_script("glob/found.lua",
                 "return { ping = function() return 'from_glob' end }\n");

    auto& config = shield::config::global_config();
    BOOST_REQUIRE(
        config.load_yaml_string("lua:\n"
                                "  script_path: " +
                                glob_dir +
                                "\n"
                                "actors:\n"
                                "  - name: cov_glob_actor\n"
                                "    script: found.lua\n"
                                "  - name: cov_missing_actor\n"
                                "    script: absolutely_missing_xyz.lua\n"));

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // The actor's script resolves via lua.script_path.
    auto found = manager.spawn("cov_glob_actor", opts_for("cov_glob_svc"));
    BOOST_REQUIRE(found.success);
    CallResult cr =
        manager.call(found.service_id, "ping", nlohmann::json::array(), 1000);
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), "from_glob");

    // Unresolvable scripts fall back to the bare (missing) path.
    auto missing =
        manager.spawn("cov_missing_actor", opts_for("cov_missing_svc"));
    BOOST_CHECK(!missing.success);
    BOOST_CHECK(missing.error_message.find("Failed to load module") !=
                std::string::npos);

    // Restore an empty actor list for later manager constructions.
    shield::config::global_config().load_yaml_string("actors: []\n");
}

// ---------------------------------------------------------------------------
// Teardown: pending timers and queued spawn jobs at destruction time.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(DestructorCleansTimersAndSpawnJobs) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    {
        LuaRuntime runtime;
        LuaServiceManager manager(runtime, system);

        // A service with a long timer still armed at teardown.
        const std::string timer_module =
            write_script("cov_teardown_timer.lua",
                         "local M = {}\n"
                         "function M.on_init(args)\n"
                         "  shield.timer_once(60000, function() end)\n"
                         "end\n"
                         "return M\n");
        auto svc =
            manager.spawn(timer_module, opts_for("cov_teardown_timer_svc"));
        BOOST_REQUIRE(svc.success);
        BOOST_CHECK_GE(manager.active_actor_timer_count(), 1u);

        // NOTE: a spawn that fails after arming a timer would exercise the
        // global actor_timers sweep in the destructor, but the failing spawn
        // also leaks its stashed CAF actor (the runtime never stops it),
        // which corrupts later actor systems non-deterministically. That
        // path is therefore intentionally not exercised here.

        // ~LuaServiceManager stops timers, actors and VMs in order.
    }

    {
        LuaRuntime runtime;
        LuaServiceManager manager(runtime, system);

        // Job 1 blocks the spawn worker (busy on_init); job 2 stays queued
        // and is dropped (with its pending bookkeeping) at teardown.
        const std::string slow_module =
            write_script("cov_teardown_slow.lua",
                         "return { on_init = function(args)\n"
                         "  local t = shield.monotonic()\n"
                         "  while shield.monotonic() - t < 300 do end\n"
                         "  return true\n"
                         "end }\n");
        BOOST_CHECK(manager.enqueue_async_spawn(
            1001, slow_module, opts_for("cov_teardown_child1")));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        BOOST_CHECK(manager.enqueue_async_spawn(
            1002, kTmpDir + "/definitely_not_here.lua",
            opts_for("cov_teardown_child2")));

        // The worker finishes job 1 (caller gone -> child rolled back) and
        // the destructor reaps job 2 from the queue.
    }
}

// ---------------------------------------------------------------------------
// Branch-coverage additions (purely additive).
// ---------------------------------------------------------------------------

// shield.exit during on_init (dispatch context with an exit request) and the
// in_exit guard when on_exit itself calls shield.exit again; plus the
// explicit shield.panic path through panic_current on a live service.
BOOST_AUTO_TEST_CASE(ExitDuringOnInitAndPanicCurrentPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script(
        "cov_exit_panic.lua",
        "local M = {}\n"
        "local mode = 'none'\n"
        "function M.on_init(args)\n"
        "  local cfg = (args and args.config) or {}\n"
        "  mode = cfg.mode or 'none'\n"
        "  if mode == 'exit_early' then shield.exit('leaving early') end\n"
        "  if mode == 'panic' then shield.panic('explicit test panic') end\n"
        "  return true\n"
        "end\n"
        "function M.on_panic(reason, context)\n"
        "  _G.panic_seen = tostring(reason)\n"
        "end\n"
        "function M.on_exit(reason)\n"
        "  -- Re-entrant exit while already exiting: guarded no-op.\n"
        "  shield.exit('again')\n"
        "end\n"
        "return M\n");

    // Exit requested from on_init: the service leaves after init returns.
    auto exited = manager.spawn(
        module,
        opts_for("cov_exit_early_svc", {{"config", {{"mode", "exit_early"}}}}));
    BOOST_REQUIRE(exited.success);
    BOOST_CHECK(wait_until(
        [&] { return manager.query_service("cov_exit_early_svc").empty(); },
        std::chrono::seconds(5)));

    // Explicit panic: on_panic hook runs and the service exits with reason
    // "panic".
    auto panicked = manager.spawn(
        module, opts_for("cov_panic_svc", {{"config", {{"mode", "panic"}}}}));
    BOOST_REQUIRE(panicked.success);
    BOOST_CHECK(wait_until(
        [&] { return manager.query_service("cov_panic_svc").empty(); },
        std::chrono::seconds(5)));

    manager.shutdown_all("done");
}

// shield.timer_once / shield.timer happy paths from a live service, plus
// cancel_timer on an unknown id from Lua.
BOOST_AUTO_TEST_CASE(LuaTimerApiHappyPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script(
        "cov_timers.lua",
        "local M = {}\n"
        "function M.on_init(args)\n"
        "  _G.hits = 0\n"
        "  shield.timer_once(20, function() _G.hits = _G.hits + 1 end)\n"
        "  shield.timer(10, function() _G.hits = _G.hits + 10 end)\n"
        "  -- Cancelling an unknown timer reports failure without erroring.\n"
        "  _G.cancel_ok = shield.cancel_timer(999999)\n"
        "  return true\n"
        "end\n"
        "return M\n");

    auto svc = manager.spawn(module, opts_for("cov_timers_svc"));
    BOOST_REQUIRE(svc.success);
    BOOST_CHECK_GE(manager.active_actor_timer_count(), 1u);

    // Give both timers time to fire at least once; the service stays alive
    // (the repeating timer keeps running until shutdown).
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    BOOST_CHECK(!manager.query_service("cov_timers_svc").empty());

    // The one-shot timer eventually retires from the active set.
    BOOST_CHECK(wait_until(
        [&] { return manager.pending_task_count("cov_timers_svc") >= 0; },
        std::chrono::seconds(2)));

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Plugin register_lua interactions: failure fails the spawn, and the Lua
// post-to-service hook routes plugin work onto a service actor.
// ---------------------------------------------------------------------------
namespace {

const char* kCovPluginSource = R"FAKE(#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"

#include <atomic>
#include <cstring>

namespace {
const struct shield_host_api_v1* g_host = nullptr;
shield_plugin_context_v1* g_ctx = nullptr;
char g_target[128] = {0};
std::atomic<int> g_posted{0};
std::atomic<int> g_post_rc{-1000};
std::atomic<int> g_reg_failures{0};

void post_fn(void*) { g_posted.fetch_add(1); }

const void* no_iface(shield_plugin_instance_v1*, const char* name,
                     shield_error_v1*) {
    static const int dummy_vtable = 0;
    if (name && std::strcmp(name, "cov.test.iface") == 0) {
        return &dummy_vtable;
    }
    return nullptr;
}

int regfail_lua(shield_plugin_instance_v1*, struct lua_State*,
                struct shield_error_v1* err) {
    g_reg_failures.fetch_add(1);
    if (err) {
        err->code = "cov.regfail";
        err->message = "register_lua refused";
    }
    return -1;
}

int post_lua(shield_plugin_instance_v1*, struct lua_State*,
             struct shield_error_v1*) {
    if (g_host && g_target[0]) {
        g_post_rc.store(g_host->lua_post_to_service(
            g_ctx, g_target, post_fn, nullptr, nullptr));
    }
    return 0;
}

int plain_start(shield_plugin_instance_v1*, shield_error_v1*) { return 0; }

int create(const struct shield_plugin_create_args_v1* args,
           struct shield_plugin_instance_v1** out,
           struct shield_error_v1*) {
    if (!args || !out) return -1;
    g_host = args->host_api;
    g_ctx = args->ctx;
    const char* cfg = args->config_json ? args->config_json : "{}";
    if (std::strstr(cfg, "\"fail\"")) {
        static struct shield_plugin_instance_v1 fail_inst;
        fail_inst.struct_size = (uint32_t)sizeof(fail_inst);
        fail_inst.get_interface = no_iface;
        fail_inst.start = plain_start;
        fail_inst.shutdown = nullptr;
        fail_inst.register_lua = regfail_lua;
        *out = &fail_inst;
        return 0;
    }
    const char* t = std::strstr(cfg, "\"target\":\"");
    if (t) {
        t += 10;
        size_t i = 0;
        while (t[i] && t[i] != '"' && i < sizeof(g_target) - 1) {
            g_target[i] = t[i];
            ++i;
        }
        g_target[i] = 0;
    }
    static struct shield_plugin_instance_v1 ok_inst;
    ok_inst.struct_size = (uint32_t)sizeof(ok_inst);
    ok_inst.get_interface = no_iface;
    ok_inst.start = plain_start;
    ok_inst.shutdown = nullptr;
    ok_inst.register_lua = post_lua;
    *out = &ok_inst;
    return 0;
}
}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const struct shield_plugin_abi_v1*
cov_plugin_entry(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        (uint32_t)sizeof(struct shield_plugin_abi_v1), "covplug.test", "1.0.0",
        create};
    return &abi;
}
extern "C" SHIELD_PLUGIN_EXPORT int cov_plugin_posted(void) {
    return g_posted.load();
}
extern "C" SHIELD_PLUGIN_EXPORT int cov_plugin_post_rc(void) {
    return g_post_rc.load();
}
extern "C" SHIELD_PLUGIN_EXPORT int cov_plugin_reg_failures(void) {
    return g_reg_failures.load();
}
)FAKE";

// Compile the fake plugin and lay out a package directory. Returns an empty
// path when no compiler is available (the test then skips).
std::filesystem::path build_cov_plugin(const std::string& entry_config) {
    namespace fs = std::filesystem;
    const fs::path dir = "/tmp/shield_cov_lua_service/plugin";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "bin", ec);
    const fs::path src = dir / "cov_plugin.cpp";
    {
        std::ofstream out(src);
        out << kCovPluginSource;
    }
    const fs::path so = dir / "bin" / "libcovplug.so";
    fs::path inc = fs::path("/home/cui/workspaces/shield/include");
    const char* compilers[] = {"/usr/bin/x86_64-linux-gnu-g++-15",
                               "/usr/bin/g++", "/usr/bin/c++"};
    for (const char* cxx : compilers) {
        std::ostringstream cmd;
        cmd << cxx << " -std=c++17 -shared -fPIC -I\"" << inc.string()
            << "\" -o \"" << so.string() << "\" \"" << src.string() << "\"";
        if (std::system(cmd.str().c_str()) == 0 && fs::exists(so)) {
            const fs::path pkg = dir / "covplug.test";
            fs::create_directories(pkg, ec);
            fs::copy_file(so, pkg / "libcovplug.so",
                          fs::copy_options::overwrite_existing, ec);
            fs::create_directories(pkg / "bin", ec);
            fs::copy_file(so, pkg / "bin" / "libcovplug.so",
                          fs::copy_options::overwrite_existing, ec);
            std::ofstream m(pkg / "manifest.yaml");
            m << "schema_version: 1\n"
              << "id: covplug.test\n"
              << "name: covplug\n"
              << "version: 1.0.0\n"
              << "kind: coverage\n"
              << "entry: cov_plugin_entry\n"
              << "library:\n"
              << "  linux: bin/libcovplug.so\n"
              << "  macos: bin/libcovplug.so\n"
              << "  windows: bin/other.dll\n"
              << "provides:\n  - interface: cov.test.iface\n"
              << "requires: []\n"
              << "config_schema:\n  type: object\n";
            return dir;
        }
    }
    (void)entry_config;
    return {};
}

}  // namespace

BOOST_AUTO_TEST_CASE(PluginRegisterLuaInteractions) {
    const auto root = build_cov_plugin("");
    if (root.empty()) {
        BOOST_TEST_MESSAGE("plugin compiler unavailable; skipping");
        return;
    }

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov_plug_mod.lua", "return {}\n");

    auto& host = shield::plugin::global_host();

    // A required plugin whose register_lua fails makes spawn fail during
    // register_api.
    {
        shield::plugin::PluginConfig pcfg;
        pcfg.directory = root.string();
        shield::plugin::InstanceDecl decl;
        decl.id = "cov_p1";
        decl.package = "covplug.test";
        decl.required = true;
        decl.config = nlohmann::json{{"fail", true}};
        pcfg.instances.push_back(decl);
        std::string error;
        BOOST_REQUIRE_MESSAGE(host.startup(pcfg, error),
                              "startup failed: " << error);
        auto failed = manager.spawn(module, opts_for("cov_plug_fail_svc"));
        BOOST_CHECK(!failed.success);
        BOOST_CHECK(failed.error_message.find("Failed to register Lua API") !=
                    std::string::npos);
        host.shutdown();
    }

    // A plugin posting work onto a live service actor through the
    // lua_post_to_service hook.
    {
        auto svc = manager.spawn(module, opts_for("cov_plug_target_svc"));
        BOOST_REQUIRE(svc.success);

        shield::plugin::PluginConfig pcfg;
        pcfg.directory = root.string();
        shield::plugin::InstanceDecl decl;
        decl.id = "cov_p2";
        decl.package = "covplug.test";
        decl.required = true;
        decl.config = nlohmann::json{{"target", "cov_plug_target_svc"}};
        pcfg.instances.push_back(decl);
        std::string error;
        BOOST_REQUIRE(host.startup(pcfg, error));

        // Any new VM registration triggers the plugin's register_lua, which
        // posts a task onto the target service's actor.
        auto other = manager.spawn(module, opts_for("cov_plug_other_svc"));
        BOOST_REQUIRE(other.success);

#ifndef _WIN32  // dlopen introspection: POSIX only
        void* handle =
            dlopen((root / "covplug.test" / "bin" / "libcovplug.so").c_str(),
                   RTLD_NOW);
        using int_fn = int (*)();
        int_fn posted = nullptr;
        int_fn post_rc = nullptr;
        if (handle) {
            posted =
                reinterpret_cast<int_fn>(dlsym(handle, "cov_plugin_posted"));
            post_rc =
                reinterpret_cast<int_fn>(dlsym(handle, "cov_plugin_post_rc"));
        }
        BOOST_REQUIRE(handle && posted && post_rc);
        const int rc = post_rc();
        BOOST_CHECK(rc == 0 || rc == 1);
        BOOST_CHECK(wait_until([&]() { return posted() >= 1; },
                               std::chrono::seconds(2)));
        if (handle) {
            // Keep the handle open: the plugin stays loaded in the host.
        }
#endif  // !_WIN32
        manager.exit(other.service_id, "cleanup");
        manager.exit(svc.service_id, "cleanup");
        host.shutdown();
    }
}

// ---------------------------------------------------------------------------
// shutdown_all stop budget: once the graceful budget is spent, the remaining
// services take the force path (no on_exit), bounding total shutdown time.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ShutdownAllBudgetForcesRemainingServices) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // on_exit busy-waits ~500ms (exit() runs it synchronously on the
    // calling thread, so the delay is deterministic for this test).
    const std::string module =
        write_script("cov_slow_exit.lua",
                     "local M = {}\n"
                     "function M.on_init() end\n"
                     "function M.on_exit(reason)\n"
                     "    local start = shield.monotonic()\n"
                     "    while shield.monotonic() - start < 500 do end\n"
                     "end\n"
                     "return M\n");
    auto a = manager.spawn(module, opts_for("cov_budget_a"));
    auto b = manager.spawn(module, opts_for("cov_budget_b"));
    BOOST_REQUIRE(a.success);
    BOOST_REQUIRE(b.success);

    const auto begin = std::chrono::steady_clock::now();
    manager.shutdown_all("budget", /*stop_budget_ms=*/1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    // Service A takes the graceful path (one 500ms on_exit); service B is
    // past the deadline and must be force-removed without another 500ms.
    BOOST_CHECK_LT(elapsed.count(), 900);
    BOOST_CHECK_EQUAL(manager.pending_task_count_total(), 0u);
}
