// Coverage tests (round 2) for src/lua/lua_api.cpp: conversion helpers,
// main-thread sync call paths, httpd verb registration, plugin query APIs,
// deadline propagation, and shield.client primitive rejections.
#define BOOST_TEST_MODULE CovLuaApi2
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_api2";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

bool run_script(sol::state& lua, const std::string& code) {
    auto result = lua.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error e = result;
        std::fprintf(stderr, "lua error: %s\n", e.what());
        return false;
    }
    return true;
}

nlohmann::json opts_for(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
}

// Echo callee used for deadline / sync-call scenarios.
const char* kCalleeScript = R"lua(
local M = {}
function M.echo(ctx, v) return v end
function M.deadline_of(ctx) return shield.deadline() end
function M.sender_of(ctx) return shield.sender() end
function M.trace_of(ctx) return shield.trace() end
return M
)lua";

// Caller service: issues shield.call_timeout so the dispatched message
// carries a deadline; the callee reports its view of shield.deadline().
const char* kDeadlineCaller = R"lua(
local M = {}
local state = {}
function M.probe(ctx, target)
  local ok, v = shield.call_timeout(4000, target, "deadline_of")
  state.ok = ok
  state.deadline = tonumber(v) or -1
  return ok
end
function M.probe_main_thread(ctx, target)
  local ok, err = shield._sync_call_timeout(50, target, "echo", "x")
  return ok, err and err.code or nil
end
function M.invalid_method_call(ctx, target)
  local ok, err = shield._sync_call_timeout(500, target, "on_reserved")
  return ok, err and err.code or nil
end
function M.dead_call(ctx, target)
  local ok, err = shield._sync_call_timeout(500, target, "echo", 1)
  state.dead_code = err and err.code or nil
  return ok
end
function M.dead_send(ctx, target)
  local ok, err = shield.send(target, "echo", 1)
  state.send_code = err and err.code or nil
  return ok, state.send_code
end
function M.numeric_target(ctx)
  local ok, err = shield.send(42, "echo")
  return ok, err and err.code or nil
end
function M.handle_target(ctx)
  local h = shield._make_handle("ghost_svc")
  local ok, err = shield.send(h, "echo")
  return ok, err and err.code or nil
end
function M.mixed_table_call(ctx, target)
  local ok, v = shield.call(target, "echo", {k = 1, [7] = "x", 2.5, true})
  return ok, type(v)
end
function M.get(ctx) return state.ok, state.deadline, state.dead_code end
return M
)lua";

// Registers routes through the remaining httpd verbs.
const char* kHttpdVerbsScript = R"lua(
local M = {}
function M.on_init()
    shield.httpd.put("/put", function(req) return "put_ok" end)
    shield.httpd.delete("/delete", function(req) return "delete_ok" end)
    shield.httpd.patch("/patch", function(req) return "patch_ok" end)
    shield.httpd.get("/after", function(req) return "late_ok" end)
end
return M
)lua";

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

}  // namespace

// ---------------------------------------------------------------------------
// Conversion helper branches.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaToJsonMixedAndUnsupportedValues) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);

    // Mixed keys force the object path; integer keys use the int branch.
    sol::table mixed = lua.create_table();
    mixed["a"] = 1;
    mixed[7] = "x";
    nlohmann::json out;
    BOOST_CHECK(lua_to_json(sol::object(mixed), &out));
    BOOST_CHECK(out.is_object());
    BOOST_CHECK_EQUAL(out["a"], 1);
    BOOST_CHECK_EQUAL(out["7"], "x");

    // Function values are not convertible; the direct form returns the
    // "<unsupported>" marker string without throwing.
    sol::object fn = lua.script("return function() end");
    nlohmann::json fn_json = lua_to_json(fn);
    BOOST_CHECK_EQUAL(fn_json, "<unsupported>");

    // Boolean members inside a contiguous array.
    sol::object arr = lua.script("return {true, false, 's'}");
    nlohmann::json arr_json = lua_to_json(arr);
    BOOST_CHECK(arr_json.is_array());
    BOOST_CHECK_EQUAL(arr_json.size(), 3u);
    BOOST_CHECK(arr_json[0].get<bool>());
    BOOST_CHECK(!arr_json[1].get<bool>());

    // json_to_lua with a value that only fits unsigned.
    lua["u"] = json_to_lua(lua, nlohmann::json(18446744073709551615ULL));
    BOOST_CHECK(run_script(lua, "assert(math.type(u) == 'integer')"));
}

// ---------------------------------------------------------------------------
// Main-thread synchronous call paths and error-code mapping.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SyncCallErrorCodes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    // Call to a service that does not exist.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield._sync_call_timeout("
                           "100, 'ghost', 'echo', 1)\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'service_not_found')"));

    // send() with a reserved method name is rejected before dispatch
    // (method validation happens ahead of the service lookup on send).
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('ghost', 'on_reserved')\n"
                   "assert(ok == false)\n"
                   "assert(err.code == 'invalid_method')"));

    // send() with a non-handle, non-string target.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield.send(42, 'echo')\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'invalid_target')"));

    // send() via a ServiceHandle built from an unknown id.
    BOOST_CHECK(run_script(lua,
                           "local h = shield._make_handle('ghost_svc')\n"
                           "local ok, err = shield.send(h, 'echo')\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'service_not_found')"));

    // Deadline/trace probes outside a dispatch context.
    BOOST_CHECK(run_script(lua,
                           "assert(shield.deadline() == nil)\n"
                           "assert(shield.trace() == nil)"));
}

// ---------------------------------------------------------------------------
// Service-level scenarios: deadline propagation, dead-service codes, httpd
// verb registration.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(DeadlineAndDeadServiceCodes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path =
        write_script("cov2_callee.lua", kCalleeScript);
    const std::string caller_path =
        write_script("cov2_deadline_caller.lua", kDeadlineCaller);

    auto callee = manager.spawn(callee_path, opts_for("cov2_callee").dump());
    BOOST_REQUIRE(callee.success);
    auto victim = manager.spawn(callee_path, opts_for("cov2_victim").dump());
    BOOST_REQUIRE(victim.success);
    auto caller = manager.spawn(caller_path, opts_for("cov2_caller").dump());
    BOOST_REQUIRE(caller.success);

    // Deadline: call_timeout does not propagate a deadline budget to the
    // callee (only the pending-call timeout is armed), so the callee's
    // shield.deadline() is nil and the caller records -1.
    {
        auto res = manager.call(caller.service_id, "probe",
                                nlohmann::json::array({callee.service_id}));
        BOOST_REQUIRE(res.success);
    }
    {
        auto res =
            manager.call(caller.service_id, "get", nlohmann::json::array());
        BOOST_REQUIRE(res.success);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(res.values[0].get<bool>());
        BOOST_CHECK_EQUAL(res.values[1].get<int64_t>(), -1);
    }

    // Calls to an exited service report service_not_found through the sync
    // call path (no tombstone distinction), while send() to the recently
    // exited service maps to service_dead via the exit tombstone.
    manager.exit(victim.service_id, "done");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        auto res = manager.call(caller.service_id, "dead_call",
                                nlohmann::json::array({victim.service_id}));
        BOOST_REQUIRE(res.success);
    }
    {
        auto res = manager.call(caller.service_id, "dead_send",
                                nlohmann::json::array({victim.service_id}));
        BOOST_REQUIRE(res.success);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(!res.values[0].get<bool>());
        BOOST_CHECK_EQUAL(res.values[1].get<std::string>(), "service_dead");
    }
    {
        auto res =
            manager.call(caller.service_id, "get", nlohmann::json::array());
        BOOST_REQUIRE(res.success);
        BOOST_CHECK_EQUAL(res.values[2].get<std::string>(),
                          "service_not_found");
    }

    // Mixed-table argument conversion through a real call.
    {
        auto res = manager.call(caller.service_id, "mixed_table_call",
                                nlohmann::json::array({callee.service_id}));
        BOOST_REQUIRE(res.success);
        BOOST_CHECK(res.values[0].get<bool>());
    }
}

BOOST_AUTO_TEST_CASE(HttpdVerbsRegisterRoutes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov2_httpd_verbs.lua", kHttpdVerbsScript);
    auto svc = manager.spawn(path, opts_for("cov2_httpd_verbs").dump());
    BOOST_REQUIRE(svc.success);

    BOOST_CHECK(runtime.find_http_route("PUT", "/put"));
    BOOST_CHECK(runtime.find_http_route("DELETE", "/delete"));
    BOOST_CHECK(runtime.find_http_route("PATCH", "/patch"));
    BOOST_CHECK(runtime.find_http_route("GET", "/after"));
    BOOST_CHECK_EQUAL(runtime.http_route_count(), 4u);

    // Exit the service before teardown: exit() drops its shield.httpd
    // routes, otherwise ~LuaRuntime would release sol::function references
    // that are bound to the (already closed) service VM.
    manager.exit(svc.service_id, "done");
}

// ---------------------------------------------------------------------------
// shield.plugin query API against a real started plugin instance.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(PluginQueryApiWithLiveInstance) {
    namespace fs = std::filesystem;
    const fs::path workdir = fs::current_path();
    const fs::path plugin_lib = workdir / "test_plugins" / "minimal.test" /
                                "bin" / "libshield_minimal_test_plugin.so";
    if (!fs::exists(plugin_lib)) {
        // Fixture not built in this tree; the plugin tests cover it.
        return;
    }

    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    shield::plugin::PluginConfig pc;
    pc.directory = (workdir / "test_plugins").string();
    shield::plugin::InstanceDecl decl;
    decl.id = "cov2.minimal";
    decl.package = "minimal.test";
    decl.required = true;
    pc.instances.push_back(decl);
    shield::plugin::BindingDecl binding;
    binding.logical = "minimal.test.iface";
    binding.instance_id = "cov2.minimal";
    pc.bindings.push_back(binding);

    std::string err;
    BOOST_REQUIRE(shield::plugin::global_host().startup(pc, err));

    // list()/instance()/binding() happy paths.
    BOOST_CHECK(run_script(lua,
                           "local pkgs = shield.plugin.packages()\n"
                           "assert(#pkgs >= 1)\n"
                           "assert(pkgs[1].id == 'minimal.test')\n"
                           "assert(pkgs[1].kind == 'test')"));
    BOOST_CHECK(run_script(lua,
                           "local insts = shield.plugin.instances()\n"
                           "assert(#insts == 1)\n"
                           "assert(insts[1].id == 'cov2.minimal')"));
    BOOST_CHECK(
        run_script(lua,
                   "local inst = shield.plugin.instance('cov2.minimal')\n"
                   "assert(inst ~= nil)\n"
                   "assert(inst.package == 'minimal.test')\n"
                   "assert(inst.state == 'started')\n"
                   "assert(inst.required == true)\n"
                   "assert(shield.plugin.instance('nope') == nil)"));
    BOOST_CHECK(
        run_script(lua,
                   "local b = shield.plugin.binding('minimal.test.iface')\n"
                   "assert(b ~= nil)\n"
                   "assert(b.instance_id == 'cov2.minimal')\n"
                   "assert(b.interface == 'minimal.test.iface')"));

    shield::plugin::global_host().shutdown();
}

// ---------------------------------------------------------------------------
// shield.client primitives without a gateway: argument validation and
// missing-gateway rejection (bind additionally refuses the main coroutine).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClientPrimitivesWithoutGateway) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    // Main-coroutine guard: bind refuses before touching any state.
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.client.bind(nil, 'p1', 'target')\n"
                   "assert(ok == false)\n"
                   "assert(err.code == 'call_not_allowed_off_coroutine')"));

    // close with a non-client argument fails.
    BOOST_CHECK(
        run_script(lua, "assert(shield.client.close(nil, 'bye') == false)"));

    // close with a materialized identity but no gateway registered: the
    // egress route cannot be resolved, so the request is dropped (false).
    BOOST_CHECK(run_script(
        lua,
        "local ctx = __shield_make_client_context(1, 0, 'p1', 'ghost_gw', "
        "'json')\n"
        "assert(shield.client.close(ctx, 'bye') == false)"));

    // The same request through the raw primitive with the marker-table
    // form (the shape a client identity takes inside message payloads).
    lua["ref_marker"] =
        shield::lua::ClientContextData{"ghost_gw", 1, 0, "p1", "json"}
            .to_json();
    BOOST_CHECK(run_script(
        lua, "assert(shield._client_close(ref_marker, 'bye') == false)"));

    // Egress with no gateway: dropped regardless of payload shape.
    BOOST_CHECK(run_script(
        lua,
        "local ctx = __shield_make_client_context(1, 0, 'p1', 'ghost_gw', "
        "'json')\n"
        "assert(shield._client_egress(ctx, 7, {k = 'v'}) == false)\n"
        "assert(shield._client_egress(ref_marker, 7, 'bytes') == false)"));
}

// ---------------------------------------------------------------------------
// json_to_lua round-trip of a client-identity marker: the trusted identity
// materializes as a read-only ClientContext and serializes back unchanged.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClientContextMarkerRoundTrip) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    const nlohmann::json marker =
        shield::lua::ClientContextData{"cov_gw", 4242, 1, "player-42", "json"}
            .to_json();
    BOOST_CHECK(marker.is_object());
    BOOST_CHECK(marker.value("__shield_client_ref", false));

    lua["ctx"] = json_to_lua(sol::state_view(lua), marker);
    BOOST_CHECK(run_script(lua,
                           "assert(ctx:session_id() == 4242)\n"
                           "assert(ctx:session_epoch() == 1)\n"
                           "assert(ctx:player_id() == 'player-42')\n"
                           "assert(ctx:gateway() == 'cov_gw')\n"
                           "assert(ctx:protocol_profile_id() == 'json')\n"
                           "local ref = ctx:ref()\n"
                           "assert(ref:session_id() == 4242)"));

    // And back: the userdata serializes to the identical marker shape.
    BOOST_CHECK(lua_to_json(lua["ctx"]) == marker);
}

// ---------------------------------------------------------------------------
// Round-3 additions: conversion helpers, ServiceHandle metamethods, httpd
// error paths, and coroutine error propagation shapes.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(JsonToLuaDiscardedAndUnsignedValues) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);

    // A discarded JSON document (failed parse) converts to nil.
    const nlohmann::json discarded =
        nlohmann::json::parse("{broken", nullptr, false);
    BOOST_CHECK(discarded.is_discarded());
    sol::object nil_value = json_to_lua(lua, discarded);
    BOOST_CHECK(nil_value == sol::nil);

    // An unsigned integer that does not fit int64 keeps its value.
    sol::object u = json_to_lua(lua, nlohmann::json(18446744073709551615ULL));
    lua["u"] = u;
    BOOST_CHECK(run_script(lua, "assert(math.type(u) == 'integer')"));
}

// ServiceHandle usertype metamethods: to_string and equality.
BOOST_AUTO_TEST_CASE(ServiceHandleMetaMethods) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    BOOST_CHECK(run_script(lua,
                           "local a = shield._make_handle('svc.a')\n"
                           "local b = shield._make_handle('svc.a')\n"
                           "local c = shield._make_handle('svc.c')\n"
                           "assert(a == b)\n"
                           "assert(a ~= c)\n"
                           "assert(tostring(a):find('svc.a', 1, true))"));
}

// shield.httpd with a null manager registration context throws instead of
// crashing.
BOOST_AUTO_TEST_CASE(HttpdWithNullManagerThrows) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, nullptr, &runtime);

    BOOST_CHECK(run_script(lua,
                           "local ok, err = pcall(function()\n"
                           "  shield.httpd.get('/x', function() end)\n"
                           "end)\n"
                           "assert(ok == false)\n"
                           "assert(tostring(err):find('not available', 1, "
                           "true))"));
}

// A live service registering an invalid httpd route (path without leading
// '/') fails registration and fails the spawn.
BOOST_AUTO_TEST_CASE(HttpdInvalidRoutePathFailsSpawn) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path = write_script(
        "cov3_httpd_bad_path.lua",
        "local M = {}\n"
        "function M.on_init()\n"
        "    shield.httpd.get('no-slash', function(req) return 'x' end)\n"
        "end\n"
        "return M\n");
    auto res = manager.spawn(path, opts_for("cov3_httpd_bad").dump());
    BOOST_CHECK(!res.success);
    BOOST_CHECK(res.error_message.find("httpd") != std::string::npos ||
                res.error_message.find("path") != std::string::npos);
}

// Coroutine call error propagation: the callee has no such method, so the
// suspended caller resumes with the error string (call_error_message string
// branch) and shield.call returns false plus the message.
BOOST_AUTO_TEST_CASE(CoroutineCallErrorShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path = write_script(
        "cov3_callee.lua",
        "local M = {}\nfunction M.echo(ctx) return 1 end\nreturn M\n");
    const std::string caller_path = write_script(
        "cov3_caller.lua",
        "local M = {}\n"
        "local state = {}\n"
        "function M.call_missing(ctx, target)\n"
        "  local ok, err = shield.call(target, 'no_such_method')\n"
        "  state.ok = ok\n"
        "  state.err = err\n"
        "  return ok, err\n"
        "end\n"
        "function M.spawn_broken(ctx)\n"
        "  local h, err = shield.spawn('/tmp/shield_cov_lua_api3_nope.lua')\n"
        "  return h, err\n"
        "end\n"
        "function M.get(ctx) return state.ok, state.err end\n"
        "return M\n");

    auto callee = manager.spawn(callee_path, opts_for("cov3_callee").dump());
    BOOST_REQUIRE(callee.success);
    auto caller = manager.spawn(caller_path, opts_for("cov3_caller").dump());
    BOOST_REQUIRE(caller.success);

    {
        auto res = manager.call(caller.service_id, "call_missing",
                                nlohmann::json::array({callee.service_id}));
        BOOST_REQUIRE(res.success);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(res.values[0].get<bool>() == false);
        const std::string err = res.values[1].get<std::string>();
        BOOST_CHECK(err.find("method not found") != std::string::npos);
    }

    {
        auto res = manager.call(caller.service_id, "spawn_broken",
                                nlohmann::json::array());
        BOOST_REQUIRE(res.success);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(res.values[0].is_null());
        BOOST_CHECK(res.values[1].is_object());
        BOOST_CHECK(res.values[1].contains("code"));
        BOOST_CHECK(res.values[1].contains("message"));
    }
}

// ---------------------------------------------------------------------------
// Round-4 additions: main-thread spawn success, config number-parsing edges,
// service-context logging, and the blocking sync-call error path.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(MainThreadSpawnAndConfigEdges) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    const std::string module_path =
        write_script("cov4_spawn_target.lua", "local M = {}\nreturn M\n");

    // Main-thread shield.spawn succeeds synchronously and returns a handle.
    BOOST_CHECK(
        run_script(lua, "local h, err = shield.spawn('" + module_path +
                            "', {name = 'cov4_spawned'})\n"
                            "assert(h ~= nil,\n"
                            "  err and (tostring(err.code) .. ' ' .. "
                            "tostring(err.message)) or 'spawn failed')\n"
                            "assert(h:id():find('cov4_spawned', 1, "
                            "true))"));

    // shield.config on a float-looking string that stod rejects (throw path
    // falls through to the integer parser, which also rejects).
    shield::config::global_config().set("cov4.badnum", std::string("e999xx"));
    BOOST_CHECK(run_script(lua,
                           "assert(shield.config('cov4.badnum') == "
                           "'e999xx')"));
}

// shield.log from inside a service handler prefixes the service id; a
// blocking _sync_call_timeout to a live service with a missing method maps
// the callee error through call_error_message's string branch.
BOOST_AUTO_TEST_CASE(ServiceLogPrefixAndSyncCallError) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path = write_script(
        "cov4_callee.lua",
        "local M = {}\nfunction M.echo(ctx) return 1 end\nreturn M\n");
    const std::string caller_path = write_script(
        "cov4_caller.lua",
        "local M = {}\n"
        "local state = {}\n"
        "function M.log_it(ctx)\n"
        "  shield.log.info('hello from service')\n"
        "  return true\n"
        "end\n"
        "function M.sync_missing(ctx, target)\n"
        "  local ok, err = shield._sync_call_timeout(3000, target, 'nope')\n"
        "  state.sync_ok = ok\n"
        "  state.sync_err = err\n"
        "  return ok, err\n"
        "end\n"
        "function M.get(ctx) return state.sync_ok, state.sync_err end\n"
        "return M\n");

    auto callee = manager.spawn(callee_path, opts_for("cov4_callee").dump());
    BOOST_REQUIRE(callee.success);
    auto caller = manager.spawn(caller_path, opts_for("cov4_caller").dump());
    BOOST_REQUIRE(caller.success);

    {
        auto res =
            manager.call(caller.service_id, "log_it", nlohmann::json::array());
        BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
    }

    {
        auto res = manager.call(caller.service_id, "sync_missing",
                                nlohmann::json::array({callee.service_id}));
        BOOST_REQUIRE(res.success);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(res.values[0].get<bool>() == false);
        // err is a {code, message} table shaped by the sync-call error path.
        BOOST_CHECK(res.values[1].contains("message"));
        BOOST_CHECK(res.values[1]["message"].get<std::string>().find(
                        "method not found") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Round-5: fork limit reached from inside a handler (tasks queue while the
// handler runs on the actor, so the pending count climbs to the limit).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ForkLimitReachedInsideHandler) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string path =
        write_script("cov5_flood.lua",
                     "local M = {}\n"
                     "local state = {}\n"
                     "function M.flood(ctx)\n"
                     "  local hit = nil\n"
                     "  for i = 1, 1100 do\n"
                     "    local id, err = shield.fork(function() end)\n"
                     "    if id == nil then hit = err.code break end\n"
                     "  end\n"
                     "  state.hit = hit\n"
                     "  return hit\n"
                     "end\n"
                     "function M.get(ctx) return state.hit end\n"
                     "return M\n");
    auto svc = manager.spawn(path, opts_for("cov5_flood").dump());
    BOOST_REQUIRE(svc.success);

    auto res = manager.call(svc.service_id, "flood", nlohmann::json::array());
    BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
    BOOST_REQUIRE_EQUAL(res.values.size(), 1u);
    BOOST_CHECK_EQUAL(res.values[0].get<std::string>(), "fork_limit");
}

// shield.config on a value that neither stoll nor stod can convert: both
// catch arms run and the raw string comes back.
BOOST_AUTO_TEST_CASE(ConfigUnparseableNumberFallsBackToString) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    shield::config::global_config().set("cov6.zzz", std::string("zzz"));
    BOOST_CHECK(run_script(lua, "assert(shield.config('cov6.zzz') == 'zzz')"));
}
