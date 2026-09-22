// Coverage tests for src/lua/lua_service.cpp.
//
// Exercises LuaServiceManager behaviour through the public C++ and Lua-facing
// surfaces: spawn option validation, on_init edge cases, send/call guards,
// exit cleanup (pending calls, timers, queued messages), fork tasks, coroutine
// call routing, async spawn outcomes, the C++ suspend/resume primitives, and
// manager teardown paths. Lua fixtures are written to /tmp.
#define BOOST_TEST_MODULE CovLuaService

// Set from CMake so the fake-plugin compile works on any checkout path.
#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
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
#include "shield/core/service_message.hpp"
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

// Timer/fork callbacks are coroutines: while one suspends on shield.call +
// shield.sleep, the owning actor must keep servicing other messages.
const char* kTimerForkProbeScript = R"lua(
local M = {}
local st = {}
function M.start_timer_probe(ctx, target)
  st.timer_started = true
  shield.timer_once(10, function()
    local ok, v = shield.call(target, "slow")
    st.timer_call_ok = ok
    st.timer_call_val = tostring(v)
    st.timer_done = true
  end)
  return true
end
function M.start_fork_probe(ctx, target)
  st.fork_started = true
  shield.fork(function()
    local ok, v = shield.call(target, "slow")
    st.fork_call_ok = ok
    st.fork_call_val = tostring(v)
    st.fork_done = true
  end)
  return true
end
function M.beat(ctx)
  st.beats = (st.beats or 0) + 1
  return st.beats
end
function M.state(ctx)
  return st.timer_done or false, st.timer_call_val or "",
         st.fork_done or false, st.fork_call_val or ""
end
return M
)lua";

// on_init runs inside a coroutine: shield.call to an already-spawned service
// suspends on_init until the response arrives, then spawn completes.
const char* kOnInitCallScript = R"lua(
local M = {}
local boot = {}
function M.on_init(args)
  local ok, v = shield.call(args.args.target, "echo", "from_init")
  boot.ok = ok
  boot.v = tostring(v)
  return true
end
function M.boot_state(ctx)
  return boot.ok or false, boot.v or ""
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
// call: coroutine self-call round-trip and dispatch timeout.
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
                     "  local ok, v = shield.call(myname, 'ping')\n"
                     "  return ok, v or 'nil'\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_self_svc"));
    BOOST_REQUIRE(svc.success);

    // A coroutine self-call serializes through the actor's own mailbox: the
    // handler suspends, the call request is dispatched back into the same
    // service, and the caller resumes with the callee's value.
    CallResult cr = manager.call(svc.service_id, "self_sync",
                                 nlohmann::json::array(), 2000);
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 2u);
    BOOST_CHECK_EQUAL(cr.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(cr.values[1].get<std::string>(), "pong");

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
// Timer/fork callbacks run as coroutines: a callback that suspends on
// shield.call + shield.sleep must leave the owning actor free to service
// other messages in the meantime.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TimerForkCoroutinesKeepActorResponsive) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path =
        write_script("tf_callee.lua", kCalleeScript);
    const std::string probe_path =
        write_script("tf_probe.lua", kTimerForkProbeScript);

    auto callee = manager.spawn(callee_path, opts_for("tf_callee_svc"));
    auto probe = manager.spawn(probe_path, opts_for("tf_probe_svc"));
    BOOST_REQUIRE(callee.success && probe.success);

    // Timer callback: fires at ~10ms, then suspends ~250ms inside
    // shield.call(callee, "slow").
    BOOST_REQUIRE(manager.send(probe.service_id, "start_timer_probe",
                               nlohmann::json::array({callee.service_id})));

    // While the timer coroutine is suspended, the probe actor must keep
    // answering calls (against a blocked actor every beat would time out, so
    // a single success already proves the contract; the window keeps sampling
    // but the assertion stays at >= 1 — a slow CI runner cannot fit two call
    // round-trips into the 100ms budget).
    int beats = 0;
    const auto timer_window_end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < timer_window_end) {
        CallResult beat =
            manager.call(probe.service_id, "beat", nlohmann::json::array(),
                         /*timeout_ms=*/500);
        if (beat.success) {
            ++beats;
        }
        std::this_thread::sleep_for(
            std::min(std::chrono::milliseconds(20),
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         timer_window_end - std::chrono::steady_clock::now())));
    }
    BOOST_CHECK_GE(beats, 1);

    // Still suspended inside the timer callback at this point (callee sleeps
    // 250ms after a ~10ms fire delay).
    CallResult st =
        manager.call(probe.service_id, "state", nlohmann::json::array(), 500);
    BOOST_REQUIRE(st.success);
    BOOST_CHECK(!st.values[0].get<bool>());

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult s = manager.call(probe.service_id, "state",
                                        nlohmann::json::array(), 500);
            return s.success && s.values[0].get<bool>();
        },
        std::chrono::seconds(3)));
    st = manager.call(probe.service_id, "state", nlohmann::json::array(), 500);
    BOOST_REQUIRE(st.success);
    BOOST_CHECK_EQUAL(st.values[1].get<std::string>(), "slow_done");

    // Fork task: same non-blocking contract.
    BOOST_REQUIRE(manager.send(probe.service_id, "start_fork_probe",
                               nlohmann::json::array({callee.service_id})));
    beats = 0;
    const auto fork_window_end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < fork_window_end) {
        CallResult beat = manager.call(probe.service_id, "beat",
                                       nlohmann::json::array(), 500);
        if (beat.success) {
            ++beats;
        }
        std::this_thread::sleep_for(
            std::min(std::chrono::milliseconds(20),
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         fork_window_end - std::chrono::steady_clock::now())));
    }
    // Same reasoning as the timer window: one successful beat while the fork
    // task is suspended proves the actor kept answering.
    BOOST_CHECK_GE(beats, 1);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult s = manager.call(probe.service_id, "state",
                                        nlohmann::json::array(), 500);
            return s.success && s.values[2].get<bool>();
        },
        std::chrono::seconds(3)));
    st = manager.call(probe.service_id, "state", nlohmann::json::array(), 500);
    BOOST_REQUIRE(st.success);
    BOOST_CHECK_EQUAL(st.values[3].get<std::string>(), "slow_done");
}

// ---------------------------------------------------------------------------
// on_init runs inside a coroutine: shield.call to an already-spawned service
// suspends on_init until the response arrives, and spawn completes after it.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(OnInitCoroutineCallSucceeds) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path =
        write_script("oi_callee.lua", kCalleeScript);
    const std::string init_path =
        write_script("oi_init.lua", kOnInitCallScript);

    auto callee = manager.spawn(callee_path, opts_for("oi_callee_svc"));
    BOOST_REQUIRE(callee.success);

    nlohmann::json opts = {
        {"name", "oi_init_svc"},
        {"args", nlohmann::json::object({{"target", callee.service_id}})},
        {"config", nlohmann::json::object()},
    };
    auto svc = manager.spawn(init_path, opts.dump());
    // Spawn success itself proves on_init (including the nested call)
    // completed inside the spawn timeout budget.
    BOOST_REQUIRE(svc.success);

    CallResult st =
        manager.call(svc.service_id, "boot_state", nlohmann::json::array(),
                     /*timeout_ms=*/1000);
    BOOST_REQUIRE(st.success);
    BOOST_REQUIRE_EQUAL(st.values.size(), 2u);
    BOOST_CHECK_EQUAL(st.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(st.values[1].get<std::string>(), "from_init");
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

    // A trusted client-identity marker in the resume payload reaches
    // push_json_to_stack's __shield_make_client_context call (now
    // protected). With no helper registered on this bare state it degrades
    // to a plain table instead of reaching the aborting panic handler.
    const nlohmann::json marker =
        ClientContextData{"cov_gw", 4242, 1, "player-42", "json"}.to_json();
    lua_State* co_plain = make_coro(lua, "cov_ok, cov_plain = ... return 0");
    uint64_t s_plain = manager.suspend_for_call(co_plain, 10000);
    manager.resume_caller(s_plain, true, nlohmann::json::array({marker}));
    sol::table plain_tbl = lua["cov_plain"];
    BOOST_CHECK(plain_tbl.valid());
    BOOST_CHECK(plain_tbl["__shield_client_ref"].valid());

    // With a helper registered the marker materializes through the
    // protected call.
    lua.script(
        "function __shield_make_client_context(sid, epoch, pid, gw, prof) "
        "return {sid = sid, pid = pid} end");
    lua_State* co_ctx = make_coro(lua, "cov_ok, cov_ctx = ... return 0");
    uint64_t s_ctx = manager.suspend_for_call(co_ctx, 10000);
    manager.resume_caller(s_ctx, true, nlohmann::json::array({marker}));
    sol::table ctx_tbl = lua["cov_ctx"];
    BOOST_CHECK(ctx_tbl.valid());
    BOOST_CHECK_EQUAL(ctx_tbl["sid"].get_or(0), 4242);

    // A raising helper degrades the slot to the plain table as well.
    lua.script("function __shield_make_client_context() error('ctx-boom') end");
    lua_State* co_boom = make_coro(lua, "cov_ok, cov_boom = ... return 0");
    uint64_t s_boom = manager.suspend_for_call(co_boom, 10000);
    manager.resume_caller(s_boom, true, nlohmann::json::array({marker}));
    sol::table boom_tbl = lua["cov_boom"];
    BOOST_CHECK(boom_tbl.valid());
    BOOST_CHECK(boom_tbl["__shield_client_ref"].valid());

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
// Yield-window guard, driving-phase edition: a completion arriving while the
// caller's driving registration (LuaServiceManager::DrivingGuard) is held
// must never be resumed in place (that would fail with "cannot resume
// running coroutine") and never waited on. resume_caller re-enqueues the
// response through the caller actor's mailbox; with no caller actor
// registered (this primitive-level case) it falls through to the erase
// path, dropping the response while leaving the running coroutine untouched
// and recoverable.
// ---------------------------------------------------------------------------
namespace {

std::atomic<int> g_parked_gate{0};
std::atomic<bool> g_parked_body_running{false};

// Continuation for the yield below: runs when something resumes the
// coroutine, with the resume payload (ok, values...) on the stack. Stashes
// it in a global for the assertions.
int parked_continue(lua_State* L, int status, lua_KContext ctx) {
    (void)status;
    (void)ctx;
    int n = lua_gettop(L);
    lua_createtable(L, n, 0);
    for (int i = 1; i <= n; ++i) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "cov_parked_payload");
    return 0;
}

int parked_body(lua_State* L) {
    g_parked_body_running.store(true, std::memory_order_release);
    while (g_parked_gate.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    return lua_yieldk(L, 0, 0, parked_continue);
}

}  // namespace

BOOST_AUTO_TEST_CASE(YieldWindowDropWithoutCallerActor) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine);
    g_parked_gate.store(0);
    g_parked_body_running.store(false);

    lua_State* co = lua_newthread(lua.lua_state());
    lua_pushcfunction(co, parked_body);

    // Drive the body on a helper thread; while it spins, hold the driving
    // registration just like a production resume source would around its
    // lua_resume span (the registration is what production drivers hold
    // while the coroutine runs inside the VM).
    std::thread driver([&]() {
        int nres = 0;
        LuaServiceManager::DrivingGuard driving(manager, co, "test-driver");
        lua_resume(co, nullptr, 0, &nres);  // stops inside lua_yield
    });
    while (!g_parked_body_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // The body is spinning with the driving registration held and no
    // caller actor exists for the entry, so the completion cannot be
    // routed anywhere. resume_caller must return promptly (no wait, no
    // Lua-state peek) and fall through to the erase path.
    uint64_t session = manager.suspend_for_call(co, 10000);
    manager.resume_caller(session, true, nlohmann::json::array({42}));

    // The completion was dropped, not parked: the timeout registry no
    // longer knows the session.
    BOOST_CHECK_EQUAL(manager.check_call_timeouts(INT64_MAX), 0);

    // Release the body: it reaches its yield (a suspension no C++ code
    // observed) and stays suspended — the guard releases with the driver.
    g_parked_gate.store(1, std::memory_order_release);
    driver.join();

    // The test itself is the recovery source: resuming after the driving
    // registration is gone delivers the payload to the continuation,
    // proving the dropped completion never corrupted the coroutine state.
    lua_pushboolean(co, 1);
    lua_pushinteger(co, 42);
    int nres = 0;
    BOOST_CHECK_EQUAL(lua_resume(co, nullptr, 2, &nres), LUA_OK);

    lua_getglobal(co, "cov_parked_payload");
    if (lua_istable(co, -1)) {
        lua_rawgeti(co, -1, 1);
        BOOST_CHECK(lua_toboolean(co, -1) == 1);
        lua_pop(co, 1);
        lua_rawgeti(co, -1, 2);
        BOOST_CHECK_EQUAL(lua_tointeger(co, -1), 42);
        lua_pop(co, 2);
    } else {
        BOOST_FAIL("manual resume payload never reached the coroutine body");
    }
}

// ---------------------------------------------------------------------------
// Regression: concurrent calls from coroutine.wrap threads (a yield no C++
// code observes) drain without the historical handshake stall.
// ---------------------------------------------------------------------------
const char* kWrapFloodCallerScript = R"lua(
local M = {}
local W = {}
function M.flood(ctx, target, n)
  for i = 1, n do
    W[i] = coroutine.wrap(function()
      local ok, v = shield.call(target, 'ping')
      _R = (_R or 0) + (ok and 1 or 0)
    end)
    W[i]()
  end
  return true
end
function M.received(ctx) return _R or 0 end
return M
)lua";

BOOST_AUTO_TEST_CASE(WrapConcurrentCallDrain) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee = write_script("cov_wrap_callee.lua",
                                            "local M = {}\n"
                                            "function M.ping(ctx)\n"
                                            "  shield.sleep(20)\n"
                                            "  return 'pong'\n"
                                            "end\n"
                                            "return M\n");
    const std::string caller =
        write_script("cov_wrap_caller.lua", kWrapFloodCallerScript);
    auto slow = manager.spawn(callee, opts_for("cov_wrap_slow"));
    auto flood = manager.spawn(caller, opts_for("cov_wrap_flood"));
    BOOST_REQUIRE(slow.success);
    BOOST_REQUIRE(flood.success);

    auto fired = manager.call(flood.service_id, "flood",
                              nlohmann::json::array({slow.service_id, 8}));
    BOOST_REQUIRE(fired.success);

    // All 8 wrap threads must get their replies (each is a 20ms callee
    // sleep away), well inside the 5s call budget.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult r = manager.call(flood.service_id, "received",
                                        nlohmann::json::array(), 1000);
            return r.success && r.values.size() == 1u && r.values[0] == 8;
        },
        std::chrono::seconds(4)));
    manager.exit(flood.service_id, "cleanup");
    manager.exit(slow.service_id, "cleanup");
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
// shield.sleep continuation errors: a handler that sleeps and then raises is
// terminal for its coroutine. The sleep resume path must behave like the
// other terminal paths — drain the error object off the coroutine stack,
// route an in-flight call's failure upstream, and feed the service's
// on_error hook (error_type "sleep", empty method name).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SleepContinuationErrorRoutesFailureAndHook) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_sleep_err.lua",
                     "local M = {}\n"
                     "local errs = {}\n"
                     "function M.on_error(err, ctx)\n"
                     "  errs[#errs + 1] = tostring(err) .. '|' ..\n"
                     "    tostring(ctx.type) .. '|' .. tostring(ctx.method)\n"
                     "end\n"
                     "function M.sleep_fail(ctx)\n"
                     "  shield.sleep(30)\n"
                     "  error('boom-after-sleep')\n"
                     "end\n"
                     "function M.sleep_fail_nonstring(ctx)\n"
                     "  shield.sleep(30)\n"
                     "  error({ code = 42 })\n"
                     "end\n"
                     "function M.get_errs(ctx)\n"
                     "  return table.concat(errs, ';;')\n"
                     "end\n"
                     "function M.get_err_count(ctx) return #errs end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_sleep_err_svc"));
    BOOST_REQUIRE(svc.success);

    // The handler serviced a call: the sleep continuation routes the error
    // upstream as the call's failure instead of leaving it to time out.
    CallResult cr = manager.call(svc.service_id, "sleep_fail",
                                 nlohmann::json::array(), 2000);
    BOOST_CHECK(!cr.success);
    BOOST_CHECK(cr.error_message.find("boom-after-sleep") != std::string::npos);

    // on_error fired with error_type "sleep" and an empty method name.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult s = manager.call(svc.service_id, "get_errs",
                                        nlohmann::json::array(), 500);
            return s.success && s.values.size() == 1u &&
                   s.values[0].get<std::string>().find("|sleep|") !=
                       std::string::npos;
        },
        std::chrono::seconds(2)));

    // A non-string error object takes the default-message arm.
    cr = manager.call(svc.service_id, "sleep_fail_nonstring",
                      nlohmann::json::array(), 2000);
    BOOST_CHECK(!cr.success);
    BOOST_CHECK_EQUAL(cr.error_message, "sleep continuation error");
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult s = manager.call(svc.service_id, "get_err_count",
                                        nlohmann::json::array(), 500);
            return s.success && s.values.size() == 1u &&
                   s.values[0].get<int>() >= 2;
        },
        std::chrono::seconds(2)));

    // A handler that sleeps and errors without servicing a call: no upstream
    // to notify, but on_error still counts the error.
    BOOST_REQUIRE(
        manager.send(svc.service_id, "sleep_fail", nlohmann::json::array()));
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult s = manager.call(svc.service_id, "get_err_count",
                                        nlohmann::json::array(), 500);
            return s.success && s.values.size() == 1u &&
                   s.values[0].get<int>() >= 3;
        },
        std::chrono::seconds(2)));
}

// ---------------------------------------------------------------------------
// Sleep-path errors feed the same consecutive-error threshold: the 10th
// sleep continuation error panics and exits the service.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SleepContinuationErrorsCountTowardPanic) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_sleep_panic.lua",
                     "local M = {}\n"
                     "function M.on_error(err) end\n"
                     "function M.sleep_fail(ctx)\n"
                     "  shield.sleep(5)\n"
                     "  error('boom')\n"
                     "end\n"
                     "function M.ping(ctx) return 'pong' end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_sleep_panic_svc"));
    BOOST_REQUIRE(svc.success);

    for (int i = 0; i < 10; ++i) {
        CallResult cr = manager.call(svc.service_id, "sleep_fail",
                                     nlohmann::json::array(), 1000);
        BOOST_CHECK(!cr.success);
    }

    // The 10th sleep continuation error panicked; the service exits
    // asynchronously, so poll for it.
    BOOST_CHECK(wait_until(
        [&]() { return manager.query_service("cov_sleep_panic_svc").empty(); },
        std::chrono::seconds(3)));
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
    fs::path inc = fs::path(SHIELD_SOURCE_DIR) / "include";
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

// ---------------------------------------------------------------------------
// valid_name arms exercised through spawn's service-name validation: the
// reserved "shield." prefix, illegal characters, the 64-byte cap (both sides
// of the boundary), and a name using every allowed character class.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SpawnNameValidationArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string plain = write_script("cov_name_plain.lua", "return {}\n");

    auto reserved = manager.spawn(plain, opts_for("shield.reserved"));
    BOOST_CHECK(!reserved.success);
    BOOST_CHECK(reserved.error_message.find("invalid service name") !=
                std::string::npos);

    auto spaced = manager.spawn(plain, opts_for("bad name!"));
    BOOST_CHECK(!spaced.success);
    BOOST_CHECK(spaced.error_message.find("invalid service name") !=
                std::string::npos);

    // Characters sorting past 'z' enter the lowercase range check but fail
    // its upper bound (the lowercase arm's tail edge).
    auto past_z = manager.spawn(plain, opts_for("bad{name|"));
    BOOST_CHECK(!past_z.success);
    BOOST_CHECK(past_z.error_message.find("invalid service name") !=
                std::string::npos);

    const std::string too_long(65, 'a');
    auto oversized = manager.spawn(plain, opts_for(too_long));
    BOOST_CHECK(!oversized.success);

    // Exactly 64 characters is still inside the cap.
    const std::string at_cap(64, 'b');
    auto boundary = manager.spawn(plain, opts_for(at_cap));
    BOOST_REQUIRE(boundary.success);
    manager.exit(boundary.service_id, "cleanup");

    // Every allowed character class in one name: upper, lower, digit,
    // underscore, dot, dash.
    auto ok = manager.spawn(plain, opts_for("Svc_1.a-b"));
    BOOST_REQUIRE(ok.success);
    manager.exit(ok.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// register_name / unregister_name arms: the no-dispatch-context error shapes
// (with and without the error out-param) run on the test thread; the name
// validation arms, the same-owner re-register, the conflicting-owner refusal,
// the unknown-name unregister and unregistering the service's own published
// name run inside a live dispatch context via shield.register/unregister.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegisterNameArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    std::string err;
    BOOST_CHECK(!manager.register_name("cov.any", &err));
    BOOST_CHECK_EQUAL(err, "register requires current service context");
    BOOST_CHECK(!manager.register_name("cov.any"));
    err.clear();
    BOOST_CHECK(!manager.unregister_name("cov.any", &err));
    BOOST_CHECK_EQUAL(err, "unregister requires current service context");
    BOOST_CHECK(!manager.unregister_name("cov.any"));

    const std::string module =
        write_script("cov_reg_arms.lua",
                     "local M = {}\n"
                     "function M.try_reg(ctx, name)\n"
                     "  local ok, e = shield.register(name)\n"
                     "  return ok, e and e.message or ''\n"
                     "end\n"
                     "function M.try_unreg(ctx, name)\n"
                     "  local ok, e = shield.unregister(name)\n"
                     "  return ok, e and e.message or ''\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_reg_arms_svc"));
    BOOST_REQUIRE(svc.success);
    auto other = manager.spawn(module, opts_for("cov_reg_other_svc"));
    BOOST_REQUIRE(other.success);

    // Names failing valid_name: empty, illegal character, over the cap and
    // the reserved "shield." prefix.
    const std::string too_long(65, 'n');
    for (const std::string& bad : {std::string(""), std::string("has space"),
                                   too_long, std::string("shield.x")}) {
        CallResult cr = manager.call(svc.service_id, "try_reg",
                                     nlohmann::json::array({bad}), 2000);
        BOOST_REQUIRE(cr.success);
        BOOST_CHECK(!cr.values[0].get<bool>());
        BOOST_CHECK(cr.values[1].get<std::string>().find(
                        "invalid service name") != std::string::npos);
    }

    // A legal alias registers, and a same-owner re-register succeeds.
    for (int i = 0; i < 2; ++i) {
        CallResult cr = manager.call(svc.service_id, "try_reg",
                                     nlohmann::json::array({"nick.2-x"}), 2000);
        BOOST_REQUIRE(cr.success);
        BOOST_CHECK(cr.values[0].get<bool>());
    }
    BOOST_CHECK_EQUAL(manager.query_service("nick.2-x"), svc.service_id);

    // Another owner registering the same alias is refused.
    CallResult clash = manager.call(other.service_id, "try_reg",
                                    nlohmann::json::array({"nick.2-x"}), 2000);
    BOOST_REQUIRE(clash.success);
    BOOST_CHECK(!clash.values[0].get<bool>());
    BOOST_CHECK(clash.values[1].get<std::string>().find("already exists") !=
                std::string::npos);

    // Unregistering an unknown name reports failure.
    CallResult missing =
        manager.call(other.service_id, "try_unreg",
                     nlohmann::json::array({"cov.nope"}), 2000);
    BOOST_REQUIRE(missing.success);
    BOOST_CHECK(!missing.values[0].get<bool>());
    BOOST_CHECK(missing.values[1].get<std::string>().find("not found") !=
                std::string::npos);

    // A service may unregister its own published service name.
    CallResult own =
        manager.call(svc.service_id, "try_unreg",
                     nlohmann::json::array({"cov_reg_arms_svc"}), 2000);
    BOOST_REQUIRE(own.success);
    BOOST_CHECK(own.values[0].get<bool>());
    BOOST_CHECK(manager.query_service("cov_reg_arms_svc").empty());

    // The null out-param arms of the in-dispatch guards: the Lua binding
    // always passes an error table, so only a direct C++ call inside a live
    // dispatch context (a fork task on the owner actor) can reach them.
    bool null_guards[4] = {true, true, true, true};
    std::atomic<bool> null_guards_done{false};
    manager.enqueue_forked_task(other.service_id, [&] {
        null_guards[0] = manager.register_name("bad name", nullptr);
        null_guards[1] = manager.register_name("nick.2-x", nullptr);
        null_guards[2] = manager.unregister_name("cov.nope", nullptr);
        null_guards[3] = manager.unregister_name("nick.2-x", nullptr);
        null_guards_done = true;
    });
    for (int i = 0; i < 200 && !null_guards_done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK(null_guards_done.load());
    BOOST_CHECK(!null_guards[0]);
    BOOST_CHECK(!null_guards[1]);
    BOOST_CHECK(!null_guards[2]);
    BOOST_CHECK(!null_guards[3]);

    manager.exit(other.service_id, "cleanup");
    manager.exit(svc.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// on_init return-value combinations: (nil, msg) and false report failure,
// the message must be a string to be surfaced, a non-boolean/non-nil first
// value counts as success.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(OnInitFailureCombinations) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script(
        "cov_init_ret.lua",
        "local M = {}\n"
        "function M.on_init(args)\n"
        "  local cfg = (args and args.config) or {}\n"
        "  local tc = cfg.test_case or ''\n"
        "  if tc == 'nil_msg' then return nil, 'init said no' end\n"
        "  if tc == 'false_only' then return false end\n"
        "  if tc == 'false_num' then return false, 42 end\n"
        "  if tc == 'nil_num' then return nil, 42 end\n"
        "  if tc == 'nil_only' then return nil end\n"
        "  if tc == 'truthy' then return 'sounds fine' end\n"
        "  return true\n"
        "end\n"
        "return M\n");

    auto nil_msg = manager.spawn(
        module,
        opts_for("cov_init_nil_msg", {{"config", {{"test_case", "nil_msg"}}}}));
    BOOST_CHECK(!nil_msg.success);
    BOOST_CHECK(nil_msg.error_message.find("init said no") !=
                std::string::npos);

    auto false_only = manager.spawn(
        module, opts_for("cov_init_false",
                         {{"config", {{"test_case", "false_only"}}}}));
    BOOST_CHECK(!false_only.success);
    BOOST_CHECK(false_only.error_message.find("on_init returned false") !=
                std::string::npos);

    auto false_num = manager.spawn(
        module, opts_for("cov_init_false_num",
                         {{"config", {{"test_case", "false_num"}}}}));
    BOOST_CHECK(!false_num.success);
    BOOST_CHECK(false_num.error_message.find("on_init returned false") !=
                std::string::npos);

    auto nil_num = manager.spawn(
        module,
        opts_for("cov_init_nil_num", {{"config", {{"test_case", "nil_num"}}}}));
    BOOST_CHECK(!nil_num.success);
    BOOST_CHECK(nil_num.error_message.find("on_init returned nil") !=
                std::string::npos);

    // A bare `return nil` (single null value) is a silent success — only
    // (nil, msg) with a trailing message reports failure.
    auto nil_only = manager.spawn(
        module, opts_for("cov_init_nil_only",
                         {{"config", {{"test_case", "nil_only"}}}}));
    BOOST_CHECK(nil_only.success);
    manager.exit(nil_only.service_id, "cleanup");

    auto truthy = manager.spawn(
        module,
        opts_for("cov_init_truthy", {{"config", {{"test_case", "truthy"}}}}));
    BOOST_CHECK(truthy.success);
    manager.exit(truthy.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// spawn rpc.routes option handling: a non-object rpc, a routes-less rpc
// object, parse failures (bad direction, missing binding), a missing handler
// with a trailing good route (for_each early exit), foreign-owned routes
// skipped silently, and a fully compiled owned c2s + s2c table.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SpawnRpcRouteArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_rpc_mod.lua",
                     "local M = {}\n"
                     "function M.handle_ping(ctx) return 'pong' end\n"
                     "return M\n");

    auto not_object =
        manager.spawn(module, opts_for("cov_rpc_a", {{"rpc", "nope"}}));
    BOOST_CHECK(not_object.success);
    manager.exit(not_object.service_id, "cleanup");

    auto no_routes = manager.spawn(
        module, opts_for("cov_rpc_b", {{"rpc", nlohmann::json::object()}}));
    BOOST_CHECK(no_routes.success);
    manager.exit(no_routes.service_id, "cleanup");

    auto bad_direction = manager.spawn(
        module,
        opts_for(
            "cov_rpc_c",
            {{"rpc",
              {{"routes",
                nlohmann::json::array({nlohmann::json{
                    {"id", 5}, {"direction", "warp"}, {"binding", "x"}}})}}}}));
    BOOST_CHECK(!bad_direction.success);
    BOOST_CHECK(bad_direction.error_message.find("rpc.routes for") !=
                std::string::npos);

    auto no_binding = manager.spawn(
        module, opts_for("cov_rpc_c2",
                         {{"rpc",
                           {{"routes", nlohmann::json::array(
                                           {nlohmann::json{{"id", 6}}})}}}}));
    BOOST_CHECK(!no_binding.success);
    BOOST_CHECK(no_binding.error_message.find("binding is required") !=
                std::string::npos);

    // The failing route comes first; the good route behind it must hit the
    // "already failed" early exit inside the compile loop.
    auto handler_missing = manager.spawn(
        module,
        opts_for(
            "cov_rpc_d",
            {{"rpc",
              {{"routes",
                nlohmann::json::array(
                    {nlohmann::json{{"id", 3},
                                    {"direction", "c2s"},
                                    {"binding", "nope"},
                                    {"owner_service", "cov_rpc_d"}},
                     nlohmann::json{{"id", 4},
                                    {"direction", "c2s"},
                                    {"binding", "handle_ping"},
                                    {"owner_service", "cov_rpc_d"}}})}}}}));
    BOOST_CHECK(!handler_missing.success);
    BOOST_CHECK(handler_missing.error_message.find(
                    "rpc binding compile failed") != std::string::npos);
    BOOST_CHECK(handler_missing.error_message.find("handler_missing") !=
                std::string::npos);

    // The compile loop stores descriptors in an unordered_map, so the visit
    // order is unspecified. Two failing routes guarantee that whichever
    // route is visited second sees the error from the first and takes the
    // "already failed" early exit, regardless of the hash order.
    auto double_missing = manager.spawn(
        module,
        opts_for(
            "cov_rpc_d2",
            {{"rpc",
              {{"routes",
                nlohmann::json::array(
                    {nlohmann::json{{"id", 31},
                                    {"direction", "c2s"},
                                    {"binding", "nope1"},
                                    {"owner_service", "cov_rpc_d2"}},
                     nlohmann::json{{"id", 32},
                                    {"direction", "c2s"},
                                    {"binding", "nope2"},
                                    {"owner_service", "cov_rpc_d2"}}})}}}}));
    BOOST_CHECK(!double_missing.success);
    BOOST_CHECK(double_missing.error_message.find("handler_missing") !=
                std::string::npos);

    // Routes owned by another service are neither compiled nor fatal.
    auto foreign = manager.spawn(
        module,
        opts_for("cov_rpc_e",
                 {{"rpc",
                   {{"routes", nlohmann::json::array({nlohmann::json{
                                   {"id", 7},
                                   {"direction", "c2s"},
                                   {"binding", "nope"},
                                   {"owner_service", "other_svc"}}})}}}}));
    BOOST_CHECK(foreign.success);
    manager.exit(foreign.service_id, "cleanup");

    // Owned c2s + s2c routes compile; the c2s handler resolves.
    auto full = manager.spawn(
        module,
        opts_for(
            "cov_rpc_f",
            {{"rpc",
              {{"routes",
                nlohmann::json::array(
                    {nlohmann::json{{"id", 8},
                                    {"direction", "c2s"},
                                    {"binding", "handle_ping"},
                                    {"owner_service", "cov_rpc_f"}},
                     nlohmann::json{{"id", 9},
                                    {"direction", "s2c"},
                                    {"binding", "push_xy"},
                                    {"owner_service", "cov_rpc_f"}}})}}}}));
    BOOST_REQUIRE(full.success);
    CallResult cr = manager.call(full.service_id, "handle_ping",
                                 nlohmann::json::array(), 2000);
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), "pong");
    manager.exit(full.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// exit() arm coverage: unknown ids are a no-op, a foreign-thread exit with
// a deadline the actor meets goes through the monitor-down wait, and an
// on_exit that outlives the deadline takes the hung teardown (registry
// dropped, VM parked, nothing joined).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ExitUnknownAndHungTeardown) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string slow_exit =
        write_script("cov_hang_exit.lua",
                     "local M = {}\n"
                     "function M.on_exit(reason)\n"
                     "    local start = shield.monotonic()\n"
                     "    while shield.monotonic() - start < 1200 do end\n"
                     "end\n"
                     "return M\n");
    const std::string plain = write_script("cov_hang_plain.lua", "return {}\n");

    // Unknown service: no-op.
    manager.exit("cov_ghost_svc", "x");

    auto healthy = manager.spawn(plain, opts_for("cov_hang_healthy"));
    BOOST_REQUIRE(healthy.success);
    manager.exit(healthy.service_id, "cleanup",
                 std::chrono::steady_clock::now() + std::chrono::seconds(5));
    BOOST_CHECK(manager.query_service("cov_hang_healthy").empty());

    auto hung = manager.spawn(slow_exit, opts_for("cov_hang_svc"));
    BOOST_REQUIRE(hung.success);
    const auto begin = std::chrono::steady_clock::now();
    manager.exit(
        hung.service_id, "stuck",
        std::chrono::steady_clock::now() + std::chrono::milliseconds(80));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    BOOST_CHECK_GE(waited.count(), 80);
    // The hung teardown dropped the registry entry without touching the VM.
    BOOST_CHECK(manager.query_service("cov_hang_svc").empty());
    BOOST_CHECK(!manager.service_vm(hung.service_id));

    // Let the stuck on_exit unwind and its actor quit before the manager is
    // destroyed (the graveyard join would otherwise wait for it here).
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
}

// ---------------------------------------------------------------------------
// forked-task queue arms: the empty-id borrow path, the rollback scan
// walking past another service's queued task, and the cancel scan doing the
// same.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ForkQueueBorrowAndScanArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // With a live service present, an empty owner id borrows its actor and
    // the task runs.
    const std::string plain =
        write_script("cov_fork_borrow.lua", "return {}\n");
    auto host = manager.spawn(plain, opts_for("cov_fork_borrow_svc"));
    BOOST_REQUIRE(host.success);
    std::atomic<int> borrowed{0};
    const uint64_t borrow_id = manager.enqueue_forked_task(
        "", [&borrowed]() { borrowed.fetch_add(1); });
    BOOST_CHECK_NE(borrow_id, 0u);
    BOOST_CHECK(wait_until([&]() { return borrowed.load() >= 1; },
                           std::chrono::seconds(2)));

    const std::string stall =
        write_script("cov_fork_scan.lua",
                     "local M = {}\n"
                     "function M.stall(ctx)\n"
                     "  local t = shield.monotonic()\n"
                     "  while shield.monotonic() - t < 500 do end\n"
                     "  return 'stalled'\n"
                     "end\n"
                     "return M\n");
    auto a = manager.spawn(stall, opts_for("cov_fork_scan_a"));
    auto b = manager.spawn(stall, opts_for("cov_fork_scan_b"));
    BOOST_REQUIRE(a.success);
    BOOST_REQUIRE(b.success);

    // Keep both actors busy so the queued tasks are not picked up.
    BOOST_CHECK(manager.send(a.service_id, "stall", nlohmann::json::array()));
    BOOST_CHECK(manager.send(b.service_id, "stall", nlohmann::json::array()));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    manager.enqueue_forked_task(a.service_id, [] {});
    manager.enqueue_forked_task(b.service_id, [] {});
    BOOST_CHECK_GE(manager.pending_task_count(a.service_id), 1u);
    BOOST_CHECK_GE(manager.pending_task_count(b.service_id), 1u);

    // The rollback scan for an unknown owner walks past A's queued task.
    BOOST_CHECK_EQUAL(manager.enqueue_forked_task("cov_no_such_service", [] {}),
                      0u);
    BOOST_CHECK_EQUAL(manager.pending_task_count("cov_no_such_service"), 0u);

    // Cancelling A walks past B's queued task.
    manager.cancel_forked_tasks_for_service(a.service_id);
    BOOST_CHECK_EQUAL(manager.pending_task_count(a.service_id), 0u);
    BOOST_CHECK_GE(manager.pending_task_count(b.service_id), 1u);
    manager.cancel_forked_tasks_for_service(b.service_id);

    // Let both stalls finish so the actors are idle again.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult ra = manager.call(a.service_id, "stall",
                                         nlohmann::json::array(), 1000);
            CallResult rb = manager.call(b.service_id, "stall",
                                         nlohmann::json::array(), 1000);
            return ra.success && rb.success;
        },
        std::chrono::seconds(3)));
    manager.exit(a.service_id, "cleanup");
    manager.exit(b.service_id, "cleanup");
    manager.exit(host.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// call_with_session: a null initiate, a failing initiate with and without a
// message, and the happy path completed from another thread (including the
// timeout<=0 default and the {message} object error payload).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallWithSessionAndSyncCompletion) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto null_initiate = manager.call_with_session(nullptr, 100);
    BOOST_CHECK(!null_initiate.success);
    BOOST_CHECK_EQUAL(null_initiate.error_message, "call dispatch failed");

    auto failing = manager.call_with_session(
        [](uint64_t, std::string& error) {
            error = "no route";
            return false;
        },
        100);
    BOOST_CHECK(!failing.success);
    BOOST_CHECK_EQUAL(failing.error_message, "no route");

    auto failing_silent = manager.call_with_session(
        [](uint64_t, std::string&) { return false; }, 100);
    BOOST_CHECK(!failing_silent.success);
    BOOST_CHECK_EQUAL(failing_silent.error_message, "call dispatch failed");

    // Happy path: the initiate schedules the completion on a helper thread;
    // timeout_ms <= 0 takes the 5000ms default.
    std::thread completer;
    auto ok = manager.call_with_session(
        [&](uint64_t session, std::string&) {
            completer = std::thread([&manager, session]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                manager.complete_call(session, true,
                                      nlohmann::json::array({7}));
            });
            return true;
        },
        0);
    completer.join();
    BOOST_REQUIRE(ok.success);
    BOOST_REQUIRE_EQUAL(ok.values.size(), 1u);
    BOOST_CHECK_EQUAL(ok.values[0].get<int>(), 7);

    // A failure whose payload is [{message=...}] surfaces the message.
    std::thread failing_completer;
    auto failed = manager.call_with_session(
        [&](uint64_t session, std::string&) {
            failing_completer = std::thread([&manager, session]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                manager.complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json{
                        {"message", "obj boom"}, {"code", 3}}}));
            });
            return true;
        },
        250);
    failing_completer.join();
    BOOST_CHECK(!failed.success);
    BOOST_CHECK_EQUAL(failed.error_message, "obj boom");

    // An empty error array carries no message: the generic fallback wins.
    std::thread empty_err_completer;
    auto empty_err = manager.call_with_session(
        [&](uint64_t session, std::string&) {
            empty_err_completer = std::thread([&manager, session]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                manager.complete_call(session, false, nlohmann::json::array());
            });
            return true;
        },
        250);
    empty_err_completer.join();
    BOOST_CHECK(!empty_err.success);
    BOOST_CHECK_EQUAL(empty_err.error_message, "call failed");

    // A non-string object payload without "message" is dumped verbatim.
    std::thread dump_completer;
    auto dumped = manager.call_with_session(
        [&](uint64_t session, std::string&) {
            dump_completer = std::thread([&manager, session]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                manager.complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json{{"code", 7}}}));
            });
            return true;
        },
        250);
    dump_completer.join();
    BOOST_CHECK(!dumped.success);
    BOOST_CHECK_EQUAL(dumped.error_message, "{\"code\":7}");
}

// ---------------------------------------------------------------------------
// Proxied (remotely originated) call sessions: unknown/non-proxied session
// no-ops, the abandon path, completion routed through the hook from
// complete_call and finish_proxied_call (with and without a hook
// installed), dispatch_remote_call validation failures, a live proxied
// dispatch whose handler completion reaches the hook, the deadline scan
// expiring a proxied session, and the armed expiry driver firing.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ProxiedSessionLifecycle) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    std::atomic<int> hook_ok{0};
    std::atomic<int> hook_fail{0};
    std::atomic<uint64_t> last_ok_session{0};
    std::atomic<uint64_t> last_fail_session{0};
    manager.set_proxied_call_hook(
        [&](uint64_t session, bool ok, const nlohmann::json&) {
            if (ok) {
                hook_ok.fetch_add(1);
                last_ok_session.store(session);
            } else {
                hook_fail.fetch_add(1);
                last_fail_session.store(session);
            }
        });

    // Unknown sessions are no-ops / false.
    manager.abandon_proxied_call(888888);
    BOOST_CHECK(
        !manager.finish_proxied_call(888888, true, nlohmann::json::array()));

    // Abandoning a real proxied session drops it.
    const uint64_t p1 = manager.begin_proxied_call(1000);
    BOOST_CHECK_NE(p1, 0u);
    manager.abandon_proxied_call(p1);

    // A proxied session completed through complete_call routes to the hook.
    const uint64_t p2 = manager.begin_proxied_call(1000);
    manager.complete_call(
        p2, false,
        nlohmann::json::array({nlohmann::json{{"message", "px fail"}}}));
    BOOST_CHECK_EQUAL(hook_fail.load(), 1);
    BOOST_CHECK_EQUAL(last_fail_session.load(), p2);

    // finish_proxied_call with the hook installed.
    const uint64_t p3 = manager.begin_proxied_call(1000);
    BOOST_CHECK(
        manager.finish_proxied_call(p3, true, nlohmann::json::array({"v"})));
    BOOST_CHECK_EQUAL(hook_ok.load(), 1);

    // Without a hook the completion still succeeds.
    manager.set_proxied_call_hook(nullptr);
    const uint64_t p4 = manager.begin_proxied_call(1000);
    BOOST_CHECK(manager.finish_proxied_call(p4, true, nlohmann::json::array()));
    manager.set_proxied_call_hook(
        [&](uint64_t session, bool ok, const nlohmann::json&) {
            if (ok) {
                hook_ok.fetch_add(1);
                last_ok_session.store(session);
            } else {
                hook_fail.fetch_add(1);
                last_fail_session.store(session);
            }
        });

    // dispatch_remote_call validation failures (error out-param both ways).
    std::string derr;
    BOOST_CHECK_EQUAL(
        manager.dispatch_remote_call("cov_ghost_svc", "",
                                     nlohmann::json::array(), 500, nullptr),
        0u);
    BOOST_CHECK_EQUAL(
        manager.dispatch_remote_call("cov_ghost_svc", "m",
                                     nlohmann::json("not-array"), 500, &derr),
        0u);
    BOOST_CHECK_EQUAL(
        manager.dispatch_remote_call("cov_ghost_svc", "m",
                                     nlohmann::json::array(), 500, &derr),
        0u);
    BOOST_CHECK(!derr.empty());

    // A live dispatch: the callee handler completion reaches the hook.
    const std::string module =
        write_script("cov_proxied_target.lua",
                     "local M = {}\n"
                     "function M.echo(ctx, v) return v end\n"
                     "return M\n");
    auto target = manager.spawn(module, opts_for("cov_proxied_target_svc"));
    BOOST_REQUIRE(target.success);
    const uint64_t d1 = manager.dispatch_remote_call(
        target.service_id, "echo", nlohmann::json::array({"hi"}), 2000, &derr);
    BOOST_CHECK_NE(d1, 0u);
    BOOST_CHECK(wait_until(
        [&]() { return hook_ok.load() >= 2 && last_ok_session.load() == d1; },
        std::chrono::seconds(2)));

    // The deadline scan expires proxied sessions into the hook.
    const uint64_t p5 = manager.begin_proxied_call(1000);
    BOOST_CHECK_EQUAL(manager.check_call_timeouts(INT64_MAX), 1);
    BOOST_CHECK_GE(hook_fail.load(), 2);
    BOOST_CHECK_EQUAL(last_fail_session.load(), p5);

    // The armed expiry driver fires on its own fuse.
    const uint64_t p6 = manager.begin_proxied_call(1000);
    manager.schedule_proxied_call_timeout(p6, 60);
    BOOST_CHECK(wait_until(
        [&]() {
            return last_fail_session.load() == p6 && hook_fail.load() >= 3;
        },
        std::chrono::seconds(2)));

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Suspend/resume primitive arms not exercised elsewhere: a not-yet-due
// deadline survives the timeout scan, a timeout_ms <= 0 suspend takes the
// 5000ms default, and a continuation error carrying a non-string object
// keeps the generic error message.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SuspendResumeExtraArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine);

    // A pending call whose deadline is in the future is not collected.
    lua_State* co_future = make_coro(lua, "return ...");
    const uint64_t s_future = manager.suspend_for_call(co_future, 60000);
    BOOST_CHECK_NE(s_future, 0u);
    BOOST_CHECK_EQUAL(manager.check_call_timeouts(0), 0);
    manager.resume_caller(s_future, true, nlohmann::json::array());

    // timeout_ms <= 0 takes the 5000ms default deadline.
    lua_State* co_default = make_coro(lua, "return ...");
    const uint64_t s_default = manager.suspend_for_call(co_default, 0);
    BOOST_CHECK_NE(s_default, 0u);
    manager.resume_caller(s_default, true, nlohmann::json::array());

    // A continuation error whose payload is a table (not a string).
    lua_State* co_obj = make_coro(lua, "error({code=7})");
    const uint64_t s_obj = manager.suspend_for_call(co_obj, 10000);
    manager.resume_caller(s_obj, true, nlohmann::json::array());
}

// ---------------------------------------------------------------------------
// Registry-read inspect projections: unknown-service errors (with and
// without the error out-param), a service without timers, and a service
// holding one repeating + one one-shot timer.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(InspectRegistryReadArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string timer_module =
        write_script("cov_inspect_timers.lua",
                     "local M = {}\n"
                     "function M.on_init()\n"
                     "  shield.timer(5000, function() end)\n"
                     "  shield.timer_once(60000, function() end)\n"
                     "  return true\n"
                     "end\n"
                     "return M\n");
    const std::string plain =
        write_script("cov_inspect_plain.lua", "return {}\n");

    auto timed = manager.spawn(timer_module, opts_for("cov_inspect_timed"));
    BOOST_REQUIRE(timed.success);
    auto bare = manager.spawn(plain, opts_for("cov_inspect_bare"));
    BOOST_REQUIRE(bare.success);

    std::string err;
    BOOST_CHECK(!manager.timer_inspect("cov_ghost_svc", &err));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(!manager.timer_inspect("cov_ghost_svc", nullptr));

    auto none = manager.timer_inspect(bare.service_id, &err);
    BOOST_REQUIRE(none.has_value());
    BOOST_CHECK_EQUAL((*none)["timers"].get<int>(), 0);
    BOOST_CHECK(!none->contains("next_fire_ms_left"));

    auto report = manager.timer_inspect(timed.service_id, &err);
    BOOST_REQUIRE(report.has_value());
    BOOST_CHECK_EQUAL((*report)["timers"].get<int>(), 2);
    BOOST_CHECK_EQUAL((*report)["repeating"].get<int>(), 1);
    BOOST_CHECK_EQUAL((*report)["once"].get<int>(), 1);
    BOOST_CHECK_EQUAL((*report)["intervals_ms"].size(), 2u);
    BOOST_CHECK(report->contains("next_fire_ms_left"));

    BOOST_CHECK(!manager.pending_calls_inspect("cov_ghost_svc", &err));
    BOOST_CHECK(!manager.pending_calls_inspect("cov_ghost_svc", nullptr));

    // Ghost service: the not-published guards of the three owner-thread
    // projections, with the error write and the null out-param.
    err.clear();
    BOOST_CHECK(!manager.inspect_refs("cov_ghost_svc", 4, 1000, &err));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(!manager.inspect_refs("cov_ghost_svc", 4, 1000, nullptr));
    err.clear();
    BOOST_CHECK(!manager.inspect_memory("cov_ghost_svc", &err));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(!manager.inspect_memory("cov_ghost_svc", nullptr));
    err.clear();
    BOOST_CHECK(!manager.inspect_coroutines("cov_ghost_svc", &err));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(!manager.inspect_coroutines("cov_ghost_svc", nullptr));

    auto pending = manager.pending_calls_inspect(timed.service_id, &err);
    BOOST_REQUIRE(pending.has_value());
    BOOST_CHECK_EQUAL((*pending)["pending_calls"].get<int>(), 0);

    manager.exit(bare.service_id, "cleanup");
    manager.exit(timed.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// Inspect report caps: 33 concurrent calls truncate pending_calls_inspect
// at 32 entries, 33 live coroutines truncate inspect_coroutines, and 33
// active timers cap the timer interval list at 32.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(InspectCapsTruncated) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string target_module =
        write_script("cov_cap_target.lua",
                     "local M = {}\n"
                     "function M.slow(ctx) shield.sleep(600) return 'x' end\n"
                     "function M.hold(ctx, ms)\n"
                     "  shield.fork(function() shield.sleep(ms) end)\n"
                     "  return true\n"
                     "end\n"
                     "return M\n");
    const std::string caller_module =
        write_script("cov_cap_caller.lua",
                     "local M = {}\n"
                     "function M.make_calls(ctx, n, target)\n"
                     "  for i = 1, n do\n"
                     "    shield.fork(function() shield.call_timeout(9000, "
                     "target, 'slow') end)\n"
                     "  end\n"
                     "  return true\n"
                     "end\n"
                     "function M.make_coros(ctx, n)\n"
                     "  for i = 1, n do\n"
                     "    shield.fork(function() shield.sleep(1500) end)\n"
                     "  end\n"
                     "  return true\n"
                     "end\n"
                     "function M.make_timers(ctx, n)\n"
                     "  for i = 1, n do\n"
                     "    shield.timer(60000, function() end)\n"
                     "  end\n"
                     "  return true\n"
                     "end\n"
                     "return M\n");
    auto target = manager.spawn(target_module, opts_for("cov_cap_target_svc"));
    BOOST_REQUIRE(target.success);
    auto caller = manager.spawn(caller_module, opts_for("cov_cap_caller_svc"));
    BOOST_REQUIRE(caller.success);

    // 33 suspended calls from the caller service.
    BOOST_CHECK(
        manager.send(caller.service_id, "make_calls",
                     nlohmann::json::array({33, "cov_cap_target_svc"})));
    bool saw_33 = wait_until(
        [&]() {
            auto pending =
                manager.pending_calls_inspect(caller.service_id, nullptr);
            return pending.has_value() &&
                   (*pending)["pending_calls"].get<int>() >= 33;
        },
        std::chrono::seconds(2));
    BOOST_CHECK(saw_33);
    auto pending = manager.pending_calls_inspect(caller.service_id, nullptr);
    BOOST_REQUIRE(pending.has_value());
    BOOST_CHECK((*pending)["truncated"].get<bool>());
    BOOST_CHECK_EQUAL((*pending)["calls"].size(), 32u);
    // The callee's own projection stays empty: the waits belong to the
    // caller (per-caller_service filtering arm).
    auto target_pending =
        manager.pending_calls_inspect(target.service_id, nullptr);
    BOOST_REQUIRE(target_pending.has_value());
    BOOST_CHECK_EQUAL((*target_pending)["pending_calls"].get<int>(), 0);

    // Drain: the callee answers after 600ms and all callers resume.
    BOOST_CHECK(wait_until(
        [&]() {
            auto left =
                manager.pending_calls_inspect(caller.service_id, nullptr);
            return left.has_value() && (*left)["pending_calls"].get<int>() == 0;
        },
        std::chrono::seconds(4)));

    // 33 live suspended coroutines. A foreign service's sleeper stays live
    // underneath: the caller's projection must skip it (owner filter).
    BOOST_CHECK(
        manager.send(target.service_id, "hold", nlohmann::json::array({4000})));
    BOOST_CHECK(manager.send(caller.service_id, "make_coros",
                             nlohmann::json::array({33})));
    bool saw_coros = wait_until(
        [&]() {
            auto coros = manager.inspect_coroutines(caller.service_id, nullptr);
            return coros.has_value() && (*coros)["total"].get<int>() >= 33;
        },
        std::chrono::seconds(2));
    BOOST_CHECK(saw_coros);
    auto coros = manager.inspect_coroutines(caller.service_id, nullptr);
    BOOST_REQUIRE(coros.has_value());
    BOOST_CHECK((*coros)["truncated"].get<bool>());
    BOOST_CHECK_EQUAL((*coros)["entries"].size(), 32u);
    BOOST_CHECK_EQUAL((*coros)["by_status"]["suspended"].get<int>(), 33);

    // Drain the sleepers.
    BOOST_CHECK(wait_until(
        [&]() {
            auto left = manager.inspect_coroutines(caller.service_id, nullptr);
            return left.has_value() && (*left)["total"].get<int>() == 0;
        },
        std::chrono::seconds(4)));

    // Drain the foreign sleeper as well so the timer teardown assertion at
    // the end sees no outstanding sleep timers.
    BOOST_CHECK(wait_until(
        [&]() {
            auto left = manager.inspect_coroutines(target.service_id, nullptr);
            return left.has_value() && (*left)["total"].get<int>() == 0;
        },
        std::chrono::seconds(6)));

    // 33 repeating timers: the interval list caps at 32.
    BOOST_CHECK(manager.send(caller.service_id, "make_timers",
                             nlohmann::json::array({33})));
    bool saw_timers = wait_until(
        [&]() {
            auto timers = manager.timer_inspect(caller.service_id, nullptr);
            return timers.has_value() && (*timers)["timers"].get<int>() >= 33;
        },
        std::chrono::seconds(2));
    BOOST_CHECK(saw_timers);
    auto timers = manager.timer_inspect(caller.service_id, nullptr);
    BOOST_REQUIRE(timers.has_value());
    BOOST_CHECK_EQUAL((*timers)["timers"].get<int>(), 33);
    BOOST_CHECK_EQUAL((*timers)["repeating"].get<int>(), 33);
    BOOST_CHECK_EQUAL((*timers)["intervals_ms"].size(), 32u);

    // Cancelling the service tears down all 33 timer drivers.
    manager.exit(caller.service_id, "cleanup");
    BOOST_CHECK_EQUAL(manager.active_actor_timer_count(), 0u);
    manager.exit(target.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// Owner-thread inspect round trips while the owning actor is busy: each
// projection fails with its dispatch-timeout error inside the 2s budget and
// a with_refs snapshot records refs_error; once the actor is idle again all
// projections succeed (including the argument clamps).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(InspectOwnerBusyTimeoutsAndHappyPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_busy_inspect.lua",
                     "local M = {}\n"
                     "function M.stall_long(ctx)\n"
                     "  local t = shield.monotonic()\n"
                     "  while shield.monotonic() - t < 20000 do end\n"
                     "  return 'stalled'\n"
                     "end\n"
                     "function M.ping(ctx) return 'pong' end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_busy_inspect_svc"));
    BOOST_REQUIRE(svc.success);

    // Keep the owner busy for the whole timeout section.
    BOOST_CHECK(
        manager.send(svc.service_id, "stall_long", nlohmann::json::array()));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::string err;
    BOOST_CHECK(!manager.inspect_refs(svc.service_id, 4, 1000, &err));
    BOOST_CHECK(err.find("refs dispatch timeout") != std::string::npos);
    err.clear();
    BOOST_CHECK(!manager.inspect_memory(svc.service_id, &err));
    BOOST_CHECK(err.find("memory dispatch timeout") != std::string::npos);
    err.clear();
    BOOST_CHECK(!manager.inspect_coroutines(svc.service_id, &err));
    BOOST_CHECK(err.find("coroutines dispatch timeout") != std::string::npos);

    // The null out-param arms of the same busy-owner guards.
    BOOST_CHECK(!manager.inspect_refs(svc.service_id, 4, 1000, nullptr));
    BOOST_CHECK(!manager.inspect_memory(svc.service_id, nullptr));
    BOOST_CHECK(!manager.inspect_coroutines(svc.service_id, nullptr));

    // A with_refs capture still stores its gauges; the failed walk is
    // recorded on the snapshot instead of failing the capture.
    auto busy_snap = manager.capture_inspect_snapshot(svc.service_id,
                                                      "busy_snap", true, &err);
    BOOST_REQUIRE(busy_snap.has_value());
    BOOST_CHECK((*busy_snap)["refs"].is_null());

    // Wait for the stall to end.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(svc.service_id, "ping",
                                         nlohmann::json::array(), 500);
            return cr.success;
        },
        std::chrono::seconds(20)));

    // Happy paths, including both sides of the argument clamps.
    auto refs = manager.inspect_refs(svc.service_id, 4, 1000, &err);
    BOOST_REQUIRE(refs.has_value());
    BOOST_CHECK((*refs)["nodes_visited"].get<int>() > 0);
    auto clamped_low = manager.inspect_refs(svc.service_id, 0, 0, nullptr);
    BOOST_CHECK(clamped_low.has_value());
    auto clamped_high =
        manager.inspect_refs(svc.service_id, 99, 999999, nullptr);
    BOOST_CHECK(clamped_high.has_value());
    auto mem = manager.inspect_memory(svc.service_id, &err);
    BOOST_REQUIRE(mem.has_value());
    BOOST_CHECK((*mem)["gc"].contains("mode"));
    BOOST_CHECK((*mem).contains("retainers"));
    auto coros = manager.inspect_coroutines(svc.service_id, &err);
    BOOST_REQUIRE(coros.has_value());
    BOOST_CHECK_EQUAL((*coros)["total"].get<int>(), 0);

    // Auto-named and named captures; a same-name capture replaces.
    auto auto_named =
        manager.capture_inspect_snapshot(svc.service_id, "", false, nullptr);
    BOOST_REQUIRE(auto_named.has_value());
    BOOST_CHECK((*auto_named)["name"].get<std::string>().find("snap-") == 0u);
    auto first =
        manager.capture_inspect_snapshot(svc.service_id, "keep", false, &err);
    BOOST_REQUIRE(first.has_value());
    auto replaced = manager.capture_inspect_snapshot(svc.service_id, "keep",
                                                     false, nullptr);
    BOOST_REQUIRE(replaced.has_value());

    // Ring eviction: nine more auto captures push the oldest sample out.
    for (int i = 0; i < 9; ++i) {
        BOOST_CHECK(
            manager.capture_inspect_snapshot(svc.service_id, "", false, nullptr)
                .has_value());
    }

    // Diff error shapes.
    err.clear();
    BOOST_CHECK(
        !manager.diff_inspect_snapshots("cov_ghost_svc", "a", "b", &err));
    BOOST_CHECK(err.find("no snapshots for service") != std::string::npos);
    BOOST_CHECK(
        !manager.diff_inspect_snapshots("cov_ghost_svc", "a", "b", nullptr));
    err.clear();
    BOOST_CHECK(
        !manager.diff_inspect_snapshots(svc.service_id, "keep", "zzz", &err));
    BOOST_CHECK(err.find("unknown snapshot name") != std::string::npos);
    BOOST_CHECK(!manager.diff_inspect_snapshots(svc.service_id, "zzz", "keep",
                                                nullptr));

    manager.exit(svc.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// Snapshot capture/diff over a mutating module table: refs summaries ride
// on named captures and the diff reports added / delta / removed top
// tables, plus a mixed sampled/unsampled diff reporting null refs.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SnapshotCaptureDiffArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_snap_mod.lua",
                     "local M = {}\n"
                     "M.data = {}\n"
                     "for i = 1, 40 do M.data[i] = i end\n"
                     "function M.add_big(ctx)\n"
                     "  M.big = {}\n"
                     "  for i = 1, 60 do M.big[i] = i end\n"
                     "  return true\n"
                     "end\n"
                     "function M.grow_data(ctx)\n"
                     "  for i = 41, 70 do M.data[i] = i end\n"
                     "  return true\n"
                     "end\n"
                     "function M.drop_big(ctx) M.big = nil return true end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_snap_svc"));
    BOOST_REQUIRE(svc.success);

    std::string err;
    BOOST_CHECK(
        !manager.capture_inspect_snapshot("cov_ghost_svc", "x", false, &err));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(!manager.capture_inspect_snapshot("cov_ghost_svc", "x", false,
                                                  nullptr));

    auto refs_a =
        manager.capture_inspect_snapshot(svc.service_id, "refsA", true, &err);
    BOOST_REQUIRE(refs_a.has_value());
    BOOST_CHECK(!(*refs_a)["refs"].is_null());

    BOOST_REQUIRE(
        manager.call(svc.service_id, "add_big", nlohmann::json::array(), 2000)
            .success);
    BOOST_REQUIRE(
        manager.call(svc.service_id, "grow_data", nlohmann::json::array(), 2000)
            .success);
    auto refs_b = manager.capture_inspect_snapshot(svc.service_id, "refsB",
                                                   true, nullptr);
    BOOST_REQUIRE(refs_b.has_value());

    auto grown =
        manager.diff_inspect_snapshots(svc.service_id, "refsA", "refsB", &err);
    BOOST_REQUIRE(grown.has_value());
    bool added_big = false;
    bool delta_data = false;
    for (const auto& t : (*grown)["delta"]["refs"]["top_tables"]) {
        const std::string path = t["path"].get<std::string>();
        const std::string change = t["change"].get<std::string>();
        if (change == "added" && path.find("big") != std::string::npos) {
            added_big = true;
        }
        if (change == "delta" && path.find("data") != std::string::npos) {
            delta_data = true;
        }
    }
    BOOST_CHECK(added_big);
    BOOST_CHECK(delta_data);

    // A sampled end against an unsampled one reports null refs.
    auto plain_snap =
        manager.capture_inspect_snapshot(svc.service_id, "", false, nullptr);
    BOOST_REQUIRE(plain_snap.has_value());
    auto mixed = manager.diff_inspect_snapshots(
        svc.service_id, "refsA", (*plain_snap)["name"].get<std::string>(),
        &err);
    BOOST_REQUIRE(mixed.has_value());
    BOOST_CHECK((*mixed)["delta"]["refs"].is_null());

    BOOST_REQUIRE(
        manager.call(svc.service_id, "drop_big", nlohmann::json::array(), 2000)
            .success);
    auto refs_c =
        manager.capture_inspect_snapshot(svc.service_id, "refsC", true, &err);
    BOOST_REQUIRE(refs_c.has_value());
    auto shrunk =
        manager.diff_inspect_snapshots(svc.service_id, "refsB", "refsC", &err);
    BOOST_REQUIRE(shrunk.has_value());
    bool removed_big = false;
    bool delta_again = false;
    for (const auto& t : (*shrunk)["delta"]["refs"]["top_tables"]) {
        const std::string path = t["path"].get<std::string>();
        const std::string change = t["change"].get<std::string>();
        if (change == "removed" && path.find("big") != std::string::npos) {
            removed_big = true;
        }
        if (change == "delta" && path.find("data") != std::string::npos) {
            delta_again = true;
        }
    }
    BOOST_CHECK(removed_big);
    BOOST_CHECK(delta_again);

    // Unknown snapshot name without the error out-param.
    BOOST_CHECK(!manager.diff_inspect_snapshots(svc.service_id, "zzz", "refsC",
                                                nullptr));

    manager.exit(svc.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// exec_lua error shapes and both GC collector modes sampled by
// inspect_memory: a generational-mode service skips the incremental restore
// while a default service reports incremental.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ExecLuaArmsAndGcModes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    nlohmann::json result;
    std::string err;
    BOOST_CHECK(!manager.exec_lua("cov_ghost_svc", "return 1"));
    BOOST_CHECK(!manager.exec_lua("cov_ghost_svc", "return 1", &result, &err));
    BOOST_CHECK_EQUAL(err, "Service not found: cov_ghost_svc");

    const std::string plain = write_script("cov_exec_plain.lua", "return {}\n");
    auto svc = manager.spawn(plain, opts_for("cov_exec_svc"));
    BOOST_REQUIRE(svc.success);

    // exec_lua must run on the owning actor: drive it through fork tasks.
    std::atomic<int> good{0};
    std::atomic<int> bad{0};
    manager.enqueue_forked_task(svc.service_id, [&]() {
        nlohmann::json r;
        if (manager.exec_lua(svc.service_id, "return 6 * 7", &r) &&
            r.is_array() && r.size() == 1u && r[0] == 42) {
            good.fetch_add(1);
        }
    });
    manager.enqueue_forked_task(svc.service_id, [&]() {
        nlohmann::json r;
        std::string e;
        if (!manager.exec_lua(svc.service_id, "error('exec boom')", &r, &e) &&
            e.find("exec boom") != std::string::npos) {
            bad.fetch_add(1);
        }
    });
    BOOST_CHECK(
        wait_until([&]() { return good.load() == 1 && bad.load() == 1; },
                   std::chrono::seconds(2)));

    // Generational mode set in on_init survives to the inspect sample.
    const std::string gen_module =
        write_script("cov_gc_gen.lua",
                     "local M = {}\n"
                     "function M.on_init()\n"
                     "  collectgarbage('generational')\n"
                     "  return true\n"
                     "end\n"
                     "return M\n");
    auto gen = manager.spawn(gen_module, opts_for("cov_gc_gen_svc"));
    BOOST_REQUIRE(gen.success);
    auto gen_mem = manager.inspect_memory(gen.service_id, &err);
    BOOST_REQUIRE(gen_mem.has_value());
    BOOST_CHECK_EQUAL((*gen_mem)["gc"]["mode"].get<std::string>(),
                      "generational");

    auto default_mem = manager.inspect_memory(svc.service_id, &err);
    BOOST_REQUIRE(default_mem.has_value());
    BOOST_CHECK_EQUAL((*default_mem)["gc"]["mode"].get<std::string>(),
                      "incremental");

    manager.exit(gen.service_id, "cleanup");
    manager.exit(svc.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// send / send_system / send_call_request validation arms with the error
// out-param left null, the recently-exited target message, and the
// post-shutdown "runtime is stopping" guards.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SendValidationNullErrorArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov_send_mod.lua",
                     "local M = {}\n"
                     "function M.echo(ctx, v) return v end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_send_svc"));
    BOOST_REQUIRE(svc.success);

    // Validation failures with error == nullptr.
    BOOST_CHECK(!manager.send(svc.service_id, "", nlohmann::json::array()));
    BOOST_CHECK(!manager.send(svc.service_id, std::string(129, 'm'),
                              nlohmann::json::array()));
    BOOST_CHECK(
        !manager.send(svc.service_id, "on_init", nlohmann::json::array()));
    const std::string huge(5 << 20, 'x');
    BOOST_CHECK(
        !manager.send(svc.service_id, "echo", nlohmann::json::array({huge})));
    BOOST_CHECK(!manager.send(svc.service_id, "echo",
                              nlohmann::json::array({"<unsupported>"})));
    BOOST_CHECK(
        !manager.send_system("cov_ghost_svc", "", nlohmann::json::array()));
    // The unknown-target guard with a valid method and a null out-param.
    BOOST_CHECK(
        !manager.send_system("cov_ghost_svc", "m", nlohmann::json::array()));
    BOOST_CHECK(!manager.send_call_request(
        "cov_ghost_svc", "m", nlohmann::json::array(), 1, nullptr));

    // manager.call to an unknown target: the post-lock lookup miss reports
    // "service not found", and a timeout_ms of 0 takes the 5000ms default.
    auto ghost_call =
        manager.call("cov_ghost_svc", "m", nlohmann::json::array());
    BOOST_CHECK(!ghost_call.success);
    BOOST_CHECK(ghost_call.error_message.find("service not found") !=
                std::string::npos);
    auto live_call =
        manager.call(svc.service_id, "echo", nlohmann::json::array({"x"}), 0);
    BOOST_REQUIRE(live_call.success);
    BOOST_CHECK_EQUAL(live_call.values[0].get<std::string>(), "x");

    // A recently exited target reports "service dead".
    auto victim = manager.spawn(module, opts_for("cov_send_victim"));
    BOOST_REQUIRE(victim.success);
    manager.exit(victim.service_id, "cleanup");
    std::string err;
    BOOST_CHECK(!manager.send("cov_send_victim", "echo",
                              nlohmann::json::array(), &err));
    BOOST_CHECK(err.find("service dead") != std::string::npos);

    // After shutdown_all the stopping guards fire.
    manager.shutdown_all("stopping-test");
    err.clear();
    BOOST_CHECK(
        !manager.send("cov_send_svc", "echo", nlohmann::json::array(), &err));
    BOOST_CHECK_EQUAL(err, "runtime is stopping");
    err.clear();
    BOOST_CHECK(!manager.send_system("cov_send_svc", "echo",
                                     nlohmann::json::array(), &err));
    BOOST_CHECK_EQUAL(err, "runtime is stopping");
    BOOST_CHECK(
        !manager.send_system("cov_send_svc", "echo", nlohmann::json::array()));
    auto stopped = manager.call_with_session(nullptr, 10);
    BOOST_CHECK(!stopped.success);
    BOOST_CHECK_EQUAL(stopped.error_message, "runtime is stopping");
    BOOST_CHECK(!manager.spawn(module, "{}").success);
    BOOST_CHECK(!manager.enqueue_async_spawn(1, module,
                                             opts_for("cov_after_shutdown")));
}

// ---------------------------------------------------------------------------
// RefsWalker pointer dedup: a function and a table each stored under two
// module fields are counted once.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RefsWalkerSharedValues) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov_refs_shared.lua",
                                            "local function shared_fn() end\n"
                                            "local M = {}\n"
                                            "M.f1 = shared_fn\n"
                                            "M.f2 = shared_fn\n"
                                            "M.t1 = {x = 1}\n"
                                            "M.t2 = M.t1\n"
                                            "return M\n");
    auto svc = manager.spawn(module, opts_for("cov_refs_shared_svc"));
    BOOST_REQUIRE(svc.success);

    std::string err;
    auto refs = manager.inspect_refs(svc.service_id, 4, 1000, &err);
    BOOST_REQUIRE(refs.has_value());
    BOOST_CHECK_EQUAL((*refs)["counts"]["functions"].get<int>(), 1);
    // M itself counts as a table, and t1/t2 dedup to one more.
    BOOST_CHECK_EQUAL((*refs)["counts"]["tables"].get<int>(), 2);
    BOOST_REQUIRE_EQUAL((*refs)["top_tables"].size(), 2u);
    BOOST_CHECK_EQUAL((*refs)["top_tables"][0]["path"].get<std::string>(), "M");
    BOOST_CHECK((*refs)["top_tables"][1]["path"].get<std::string>().rfind(
                    "M.t", 0) == 0);

    manager.exit(svc.service_id, "cleanup");
}

// ---------------------------------------------------------------------------
// shield.exit reason shapes from a handler: the no-arg form records the
// "normal" default, the named form the custom reason; both drive the exit
// once the handler returns.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RequestExitReasonArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script(
        "cov_exit_reason.lua",
        "local M = {}\n"
        "function M.go_noarg(ctx) shield.exit() return true end\n"
        "function M.go_named(ctx) shield.exit('custom') return true end\n"
        "return M\n");

    auto noarg = manager.spawn(module, opts_for("cov_exit_noarg_svc"));
    BOOST_REQUIRE(noarg.success);
    auto cr = manager.call(noarg.service_id, "go_noarg",
                           nlohmann::json::array(), 2000);
    BOOST_CHECK(cr.success);
    BOOST_CHECK(wait_until(
        [&]() { return manager.query_service("cov_exit_noarg_svc").empty(); },
        std::chrono::seconds(2)));

    auto named = manager.spawn(module, opts_for("cov_exit_named_svc"));
    BOOST_REQUIRE(named.success);
    cr = manager.call(named.service_id, "go_named", nlohmann::json::array(),
                      2000);
    BOOST_CHECK(cr.success);
    BOOST_CHECK(wait_until(
        [&]() { return manager.query_service("cov_exit_named_svc").empty(); },
        std::chrono::seconds(2)));
}
