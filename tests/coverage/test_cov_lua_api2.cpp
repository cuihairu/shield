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
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/global/global_manager.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/net/session.hpp"
#include "shield/player/player_manager.hpp"
#include "shield/plugin/plugin_host.hpp"
#include "shield/server/server_manager.hpp"

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

bool run_script(shd::state& lua, const std::string& code) {
    auto result = lua.script(code);
    if (!result.valid()) {
        const shd::error e = result;
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
  local ok, err = shield.call(target, "echo", "x")
  return ok, err and err.code or nil
end
function M.invalid_method_call(ctx, target)
  local ok, err = shield.call(target, "on_reserved")
  return ok, err and err.code or nil
end
function M.dead_call(ctx, target)
  local ok, err = shield.call_timeout(500, target, "echo", 1)
  state.dead_code = err and err.code or nil
  return ok
end
function M.bad_call_target(ctx)
  local ok, err = shield.call(42, "echo")
  return ok, err and err.code or nil
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
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);

    // Mixed keys force the object path; integer keys use the int branch.
    shd::table mixed = lua.create_table();
    mixed["a"] = 1;
    mixed[7] = "x";
    nlohmann::json out;
    BOOST_CHECK(lua_to_json(shd::object(mixed), &out));
    BOOST_CHECK(out.is_object());
    BOOST_CHECK_EQUAL(out["a"], 1);
    BOOST_CHECK_EQUAL(out["7"], "x");

    // Function values are not convertible; the direct form returns the
    // "<unsupported>" marker string without throwing.
    shd::object fn = lua.script("return function() end");
    nlohmann::json fn_json = lua_to_json(fn);
    BOOST_CHECK_EQUAL(fn_json, "<unsupported>");

    // Boolean members inside a contiguous array.
    shd::object arr = lua.script("return {true, false, 's'}");
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

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    // Main-thread shield.call / shield.call_timeout are frozen to an explicit
    // error code (M4: calls only run inside coroutines).
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.call('ghost', 'echo', 1)\n"
                   "assert(ok == false)\n"
                   "assert(err.code == 'call_not_allowed_off_coroutine')\n"
                   "local ok2, err2 = shield.call_timeout(100, 'ghost', "
                   "'echo', 1)\n"
                   "assert(ok2 == false)\n"
                   "assert(err2.code == 'call_not_allowed_off_coroutine')"));

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

    // Calls to an exited service report service_not_found through the
    // coroutine call path (no tombstone distinction), while send() to the
    // recently exited service maps to service_dead via the exit tombstone.
    manager.exit(victim.service_id, "done");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        auto res = manager.call(caller.service_id, "dead_call",
                                nlohmann::json::array({victim.service_id}));
        BOOST_REQUIRE(res.success);
    }
    {
        auto res = manager.call(caller.service_id, "bad_call_target",
                                nlohmann::json::array());
        BOOST_REQUIRE(res.success);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(!res.values[0].get<bool>());
        BOOST_CHECK_EQUAL(res.values[1].get<std::string>(), "invalid_target");
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
    // routes, otherwise ~LuaRuntime would release shd::function references
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

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

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

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

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
    lua["ref_marker"] = shield::lua::json_to_lua(
        lua, shield::lua::ClientContextData{"ghost_gw", 1, 0, "p1", "json"}
                 .to_json());
    BOOST_CHECK(run_script(
        lua, "assert(shield._client_close(ref_marker, 'bye') == false)"));

    // Egress with no gateway: dropped regardless of payload shape.
    BOOST_CHECK(run_script(
        lua,
        "local ctx = __shield_make_client_context(1, 0, 'p1', 'ghost_gw', "
        "'json')\n"
        "assert(shield._client_egress(ctx, 7, {k = 'v'}) == false)\n"
        "assert(shield._client_egress(ref_marker, 7, 'bytes') == false)\n"
        // A non-client first argument never yields identity data, so the
        // request is rejected before the gateway lookup.
        "assert(shield._client_egress(nil, 7, 'bytes') == false)"));
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

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    const nlohmann::json marker =
        shield::lua::ClientContextData{"cov_gw", 4242, 1, "player-42", "json"}
            .to_json();
    BOOST_CHECK(marker.is_object());
    BOOST_CHECK(marker.value("__shield_client_ref", false));

    lua["ctx"] = json_to_lua(shd::state_view(lua), marker);
    BOOST_CHECK(run_script(lua,
                           "assert(ctx:session_id() == 4242)\n"
                           "assert(ctx:session_epoch() == 1)\n"
                           "assert(ctx:player_id() == 'player-42')\n"
                           "assert(ctx:gateway() == 'cov_gw')\n"
                           "assert(ctx:protocol_profile_id() == 'json')\n"
                           "cov_ref = ctx:ref()\n"
                           "assert(cov_ref:session_id() == 4242)"));

    // And back: the userdata serializes to the identical marker shape.
    BOOST_CHECK(lua_to_json(lua["ctx"]) == marker);

    // The sol-created ref from ctx:ref() is a pointer box (sol usertype
    // layout); it serializes through the sol-side reader to the same marker.
    BOOST_CHECK(lua_to_json(lua["cov_ref"]) == marker);

    // A shd-created context (raw payload plus the uservalue type-name tag)
    // round-trips through the shd reader to the same marker.
    shd::object shd_ctx = shd::make_object(
        shd::state_view(lua), ClientContextBox{ClientContextData{
                                  "cov_gw", 4242, 1, "player-42", "json"}});
    BOOST_CHECK(lua_to_json(shd_ctx) == marker);
}

// ---------------------------------------------------------------------------
// Round-3 additions: conversion helpers, ServiceHandle metamethods, httpd
// error paths, and coroutine error propagation shapes.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(JsonToLuaDiscardedAndUnsignedValues) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);

    // A discarded JSON document (failed parse) converts to nil.
    const nlohmann::json discarded =
        nlohmann::json::parse("{broken", nullptr, false);
    BOOST_CHECK(discarded.is_discarded());
    shd::object nil_value = json_to_lua(lua, discarded);
    BOOST_CHECK(nil_value == shd::nil);

    // An unsigned integer that does not fit int64 keeps its value.
    shd::object u = json_to_lua(lua, nlohmann::json(18446744073709551615ULL));
    lua["u"] = u;
    BOOST_CHECK(run_script(lua, "assert(math.type(u) == 'integer')"));
}

// ServiceHandle usertype metamethods: to_string and equality.
BOOST_AUTO_TEST_CASE(ServiceHandleMetaMethods) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

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

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), nullptr, &runtime);

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
        // The call wrapper shapes a non-table resume payload into the stable
        // {code, message} error form.
        BOOST_REQUIRE(res.values[1].is_object());
        BOOST_CHECK_EQUAL(res.values[1].value("code", ""), "method_not_found");
        BOOST_CHECK(
            res.values[1].value("message", "").find("method not found") !=
            std::string::npos);
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

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

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
// coroutine call to a live service with a missing method maps the callee
// error through the call wrapper's string-to-error-table shaping.
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
        "  local ok, err = shield.call_timeout(3000, target, 'nope')\n"
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

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::config::global_config().set("cov6.zzz", std::string("zzz"));
    BOOST_CHECK(run_script(lua, "assert(shield.config('cov6.zzz') == 'zzz')"));
}

// ---------------------------------------------------------------------------
// Round-6 additions (branch coverage): make_error detail variants, the
// error-code shim, the sync send error matrix, the client-context
// materializer guard arms, sparse-array argument shapes, coroutine-call
// primitive shapes, fork anchoring, config exponent parsing, the
// _client_bind empty-field guards, the client_rpc helper registration
// guards, and httpd without a runtime.
// ---------------------------------------------------------------------------

// Direct declaration (the definition's default arguments are not repeated).
namespace shield::lua {
shd::table make_error(shd::this_state state, std::string code,
                      std::string message, bool retryable, shd::object detail);
}  // namespace shield::lua

// make_error's detail branch: a valid non-nil object is attached, while a
// valid-but-nil object and an invalid object both leave the field absent.
// The error-code shim maps a "service dead" message to service_dead.
BOOST_AUTO_TEST_CASE(MakeErrorDetailAndErrorCodeShim) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state());

    BOOST_CHECK(run_script(lua,
                           "assert(shield._call_error_code('x service dead "
                           "y') == 'service_dead')\n"
                           "assert(shield._call_error_code('service not "
                           "found: z') == 'service_not_found')"));

    // Valid non-nil detail: attached to the error table.
    {
        shd::object detail =
            shd::make_object(shd::state_view(lua), "extra-context");
        shd::table err = shield::lua::make_error(
            shd::this_state(lua.lua_state()), "code_a", "msg_a", false, detail);
        BOOST_CHECK_EQUAL(err["code"].get<std::string>(), "code_a");
        BOOST_CHECK_EQUAL(err["detail"].get<std::string>(), "extra-context");
    }
    // Valid but nil detail: no detail field.
    {
        shd::object detail(shd::nil);
        shd::table err = shield::lua::make_error(
            shd::this_state(lua.lua_state()), "code_b", "msg_b", true, detail);
        BOOST_CHECK_EQUAL(err["code"].get<std::string>(), "code_b");
        BOOST_CHECK(!err["detail"].valid());
    }
    // Invalid object detail: no detail field either.
    {
        shd::object detail{};
        shd::table err = shield::lua::make_error(
            shd::this_state(lua.lua_state()), "code_c", "msg_c", false, detail);
        BOOST_CHECK_EQUAL(err["code"].get<std::string>(), "code_c");
        BOOST_CHECK(!err["detail"].valid());
    }
}

// The sync send() error chain in lua_api maps each manager error message to
// a stable code; the message-too-large and unsupported-value arms need
// payloads that trip the payload validators.
BOOST_AUTO_TEST_CASE(SyncSendErrorMatrix) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    const std::string callee_path = write_script(
        "cov7_callee.lua",
        "local M = {}\nfunction M.echo(ctx, v) return v end\nreturn M\n");
    auto callee = manager.spawn(callee_path, opts_for("cov7_callee").dump());
    BOOST_REQUIRE(callee.success);

    // Oversized string argument: exceeds kMaxMessageSize (1 MiB).
    BOOST_CHECK(run_script(
        lua, "local ok, err = shield.send('" + callee.service_id +
                 "', 'echo', string.rep('x', 1100 * 1024))\n"
                 "assert(ok == false)\n"
                 "assert(err.code == 'message_too_large', err.code)\n"
                 "assert(err.message:find('message too large', 1, true))"));

    // A function argument serializes to the <unsupported> sentinel, which
    // the payload validator rejects.
    BOOST_CHECK(run_script(
        lua, "local ok, err = shield.send('" + callee.service_id +
                 "', 'echo', function() end)\n"
                 "assert(ok == false)\n"
                 "assert(err.code == 'encode_failed', err.code)\n"
                 "assert(err.message:find('unsupported', 1, true))"));

    // A recently exited service maps to service_dead (the dead arm sits at
    // the end of the else-if chain).
    manager.exit(callee.service_id, "done");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    BOOST_CHECK(
        run_script(lua, "local ok, err = shield.send('" + callee.service_id +
                            "', 'echo', 1)\n"
                            "assert(ok == false)\n"
                            "assert(err.code == 'service_dead', err.code)"));
}

// The client-identity materializer inside json_to_lua degrades to a plain
// table whenever the installed __shield_make_client_context hook is not a
// usable function or returns nothing.
BOOST_AUTO_TEST_CASE(ClientContextMaterializerGuardArms) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), nullptr, nullptr);

    const nlohmann::json marker =
        shield::lua::ClientContextData{"ghost_gw", 7, 3, "p1", "json"}
            .to_json();
    lua["ctx_marker"] = shield::lua::json_to_lua(lua, marker);

    // Hook set to a non-function value: the is<protected_function> guard
    // fails and the marker degrades to a plain table.
    BOOST_CHECK(run_script(lua,
                           "__shield_make_client_context = 'not a "
                           "function'"));
    {
        shd::object obj = json_to_lua(shd::state_view(lua), marker);
        BOOST_CHECK(obj.is<shd::table>());
        BOOST_CHECK(obj.as<shd::table>()["gateway_address"].valid());
    }

    // Hook that errors: the protected call fails and the fallback runs.
    BOOST_CHECK(run_script(lua,
                           "__shield_make_client_context = function()\n"
                           "  error('materializer boom')\n"
                           "end"));
    {
        shd::object obj = json_to_lua(shd::state_view(lua), marker);
        BOOST_CHECK(obj.is<shd::table>());
    }

    // Hook returning nothing: return_count() == 0 and the fallback runs.
    BOOST_CHECK(
        run_script(lua, "__shield_make_client_context = function() end"));
    {
        shd::object obj = json_to_lua(shd::state_view(lua), marker);
        BOOST_CHECK(obj.is<shd::table>());
    }
}

// A Lua table with integer keys but a hole is carried as a JSON object (not
// an array), so the payload validation still passes and send() succeeds.
BOOST_AUTO_TEST_CASE(SparseIntegerKeyedArgSentAsObject) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    const std::string callee_path = write_script(
        "cov8_callee.lua",
        "local M = {}\nfunction M.echo(ctx, v) return v end\nreturn M\n");
    auto callee = manager.spawn(callee_path, opts_for("cov8_callee").dump());
    BOOST_REQUIRE(callee.success);

    // {[1]='a', [3]='b'}: all keys are positive integers (array_like holds)
    // but max_index != entry_count, so the table is emitted as an object.
    BOOST_CHECK(run_script(
        lua, "local ok, err = shield.send('" + callee.service_id +
                 "', 'echo', {[1] = 'a', [3] = 'b'})\n"
                 "assert(ok == true, err and (err.code .. ' ' .. err.message) "
                 "or 'send failed')"));
}

// Direct use of the coroutine-call primitive: an invalid target suspends and
// completes with the stable invalid_target error, while the packed-count
// variants control how many positional arguments are forwarded.
BOOST_AUTO_TEST_CASE(CoroCallPrimitiveShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path = write_script(
        "cov9_callee.lua",
        "local M = {}\nfunction M.echo(ctx, v) return v end\nreturn M\n");
    const std::string caller_path = write_script(
        "cov9_caller.lua",
        "local M = {}\n"
        "function M.prim_invalid(ctx)\n"
        "  local sid = shield._coro_call(42, 'echo', {}, 800)\n"
        "  local ok, err = coroutine.yield()\n"
        "  return sid ~= nil and sid ~= 0, ok, err and err.code or nil\n"
        "end\n"
        "function M.prim_no_n(ctx, target)\n"
        "  shield._coro_call(target, 'echo', {1}, 3000)\n"
        "  local ok, v = coroutine.yield()\n"
        "  return ok, v\n"
        "end\n"
        "function M.prim_bad_n(ctx, target)\n"
        "  shield._coro_call(target, 'echo', {n = 'x', 1}, 3000)\n"
        "  local ok, v = coroutine.yield()\n"
        "  return ok, v\n"
        "end\n"
        "function M.prim_zero_n(ctx, target)\n"
        "  shield._coro_call(target, 'echo', {n = 0}, 3000)\n"
        "  local ok, v = coroutine.yield()\n"
        "  return ok, v\n"
        "end\n"
        "return M\n");

    auto callee = manager.spawn(callee_path, opts_for("cov9_callee").dump());
    BOOST_REQUIRE(callee.success);
    auto caller = manager.spawn(caller_path, opts_for("cov9_caller").dump());
    BOOST_REQUIRE(caller.success);

    {
        auto res = manager.call(caller.service_id, "prim_invalid",
                                nlohmann::json::array());
        BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
        BOOST_REQUIRE(res.values.size() >= 3u);
        BOOST_CHECK(res.values[0].get<bool>());  // a session id was returned
        BOOST_CHECK(!res.values[1].get<bool>());
        BOOST_CHECK_EQUAL(res.values[2].get<std::string>(), "invalid_target");
    }
    {
        auto res = manager.call(caller.service_id, "prim_no_n",
                                nlohmann::json::array({callee.service_id}));
        BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(res.values[0].get<bool>());
        BOOST_CHECK_EQUAL(res.values[1].get<int>(), 1);
    }
    {
        // A non-integer "n" is ignored: the table size decides the arity.
        auto res = manager.call(caller.service_id, "prim_bad_n",
                                nlohmann::json::array({callee.service_id}));
        BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(res.values[0].get<bool>());
        BOOST_CHECK_EQUAL(res.values[1].get<int>(), 1);
    }
    {
        // n = 0 sends no positional arguments at all.
        auto res = manager.call(caller.service_id, "prim_zero_n",
                                nlohmann::json::array({callee.service_id}));
        BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK(res.values[0].get<bool>());
        BOOST_CHECK(res.values[1].is_null());
    }
}

// A function forked inside a handler coroutine is re-anchored onto the main
// thread's state; forking from the main thread (hostless) takes the
// same-state shortcut and still dispatches through a borrowed service actor.
BOOST_AUTO_TEST_CASE(ForkAnchorsInsideHandlerAndFromMainThread) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee_path = write_script(
        "cov10_callee.lua",
        "local M = {}\nfunction M.echo(ctx) return 1 end\nreturn M\n");
    const std::string caller_path =
        write_script("cov10_caller.lua",
                     "local M = {}\n"
                     "function M.fork_inside(ctx)\n"
                     "  local id, err = shield.fork(function() end)\n"
                     "  return type(id), err and err.code or nil\n"
                     "end\n"
                     "return M\n");
    auto callee = manager.spawn(callee_path, opts_for("cov10_callee").dump());
    BOOST_REQUIRE(callee.success);
    auto caller = manager.spawn(caller_path, opts_for("cov10_caller").dump());
    BOOST_REQUIRE(caller.success);

    {
        auto res = manager.call(caller.service_id, "fork_inside",
                                nlohmann::json::array());
        BOOST_REQUIRE_MESSAGE(res.success, res.error_message);
        BOOST_REQUIRE(res.values.size() >= 2u);
        BOOST_CHECK_EQUAL(res.values[0].get<std::string>(), "number");
        BOOST_CHECK(res.values[1].is_null());
    }

    BOOST_CHECK(wait_until(
        [&] {
            return manager.pending_task_count_total() == 0 &&
                   manager.active_fork_task_count() == 0;
        },
        std::chrono::seconds(10)));

    // Main-thread fork (fn's state is already the main state, so the
    // re-anchor is skipped) borrows the callee's actor and returns a task id.
    shd::state main_lua;
    main_lua.open_libraries(shd::lib::base, shd::lib::coroutine,
                            shd::lib::table, shd::lib::string, shd::lib::os,
                            shd::lib::math);
    register_full_shield_api(main_lua.lua_state(), &manager, &runtime);
    // The main chunk must END right after the fork: the borrowed actor
    // enters this bare VM from another thread as soon as it picks the task
    // up, and a lua_State must never be entered from two OS threads at
    // once — the Windows CI failure of 2026-09-23 ("attempt to concatenate
    // a string value" from a plain integer id) was exactly that race
    // stomping a TValue's tag while the chunk was still mid-assert. The id
    // is stashed globally and asserted from C++ after the drain below,
    // when the VM is idle again.
    auto fork_result = main_lua.script(
        "local id = shield.fork(function() end)\n"
        "_G.__fork_id = id\n");
    BOOST_REQUIRE(fork_result.valid());
    // Drain the execute phase too: pending_task_count_total() hits zero at
    // dequeue time, while the task body — which runs main_lua's function on
    // the borrowed actor thread — may still be in flight. Destroying
    // main_lua before the body finishes races lua_close against it.
    BOOST_CHECK(wait_until(
        [&] {
            return manager.pending_task_count_total() == 0 &&
                   manager.active_fork_task_count() == 0;
        },
        std::chrono::seconds(10)));

    // The VM is idle now: read the stashed id back and assert the fork
    // contract (a positive task id) directly from C++, naming the real
    // type via lua_typename should it ever regress.
    shd::object fork_id = main_lua["_G"]["__fork_id"];
    shd::type fork_id_type = fork_id.get_type();
    BOOST_CHECK_MESSAGE(
        fork_id_type == shd::type::number,
        "fork id type: " << lua_typename(main_lua.lua_state(),
                                         static_cast<int>(fork_id_type)));
    if (fork_id_type == shd::type::number) {
        BOOST_CHECK(fork_id.as<lua_Integer>() > 0);
    }
}

// shield.config parses exponent-notation floats when the whole string is
// consumed, including the capital-E variant.
BOOST_AUTO_TEST_CASE(ConfigExponentNotationParsesAsFloat) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state());

    shield::config::global_config().set("cov11.exponent", std::string("1E3"));
    BOOST_CHECK(
        run_script(lua, "assert(shield.config('cov11.exponent') == 1000.0)"));
}

// The _client_bind primitive refuses (returns 0, no suspension) when the
// player id or the target service is empty. The client argument is a valid
// marker table, so the 0 returns come from the empty-field guards and not
// from client-argument rejection.
BOOST_AUTO_TEST_CASE(ClientBindEmptyFieldGuards) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), nullptr, nullptr);

    BOOST_CHECK(
        run_script(lua,
                   "ctx_marker = {__shield_client_ref = true,\n"
                   "  gateway_address = 'ghost_gw', session_id = 7,\n"
                   "  session_epoch = 3, player_id = 'p1',\n"
                   "  protocol_profile_id = 'json'}\n"
                   "assert(shield._client_bind(ctx_marker, '', 'target', "
                   "100) == 0)\n"
                   "assert(shield._client_bind(ctx_marker, 'p1', '', 100) == "
                   "0)"));
}

// register_client_rpc_helper: the route-name reverse map is created on
// demand, repaired when clobbered with a non-table, and reused when already
// valid. The registered helper rejects a non-client argument.
BOOST_AUTO_TEST_CASE(RegisterClientRpcHelperGuardArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, nullptr);

    // Fresh state: the helper creates the reverse-name table (valid-arm).
    register_client_rpc_helper(lua, &manager, "push_msg", 41);
    BOOST_CHECK(run_script(lua,
                           "assert(shield._client_route_names[41] == "
                           "'push_msg')\n"
                           "assert(type(shield.client_rpc.push_msg) == "
                           "'function')\n"
                           "assert(shield.client_rpc.push_msg(nil, {}) == "
                           "false)"));

    // Clobbered reverse map (valid table but not a table): repaired.
    BOOST_CHECK(run_script(lua, "shield._client_route_names = 17"));
    register_client_rpc_helper(lua, &manager, "kick_msg", 42);
    BOOST_CHECK(run_script(lua,
                           "assert(shield._client_route_names[42] == "
                           "'kick_msg')\n"
                           "assert(shield._client_route_names[41] == nil)"));

    // Already-valid reverse map: reused without recreation.
    register_client_rpc_helper(lua, &manager, "pong_msg", 43);
    BOOST_CHECK(run_script(lua,
                           "assert(shield._client_route_names[43] == "
                           "'pong_msg')\n"
                           "assert(shield._client_route_names[42] == "
                           "'kick_msg')"));
}

// httpd route registration without a runtime is rejected with the stable
// "not available" error even when a manager is present.
BOOST_AUTO_TEST_CASE(HttpdWithoutRuntimeThrows) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, nullptr);

    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = pcall(function()\n"
                   "  shield.httpd.get('/no-runtime', function() end)\n"
                   "end)\n"
                   "assert(ok == false)\n"
                   "assert(tostring(err):find('not available', 1, true))"));
}

// ---------------------------------------------------------------------------
// Round-7 additions (branch coverage): player-ref marker decode and the
// table-ref epoch shapes, cluster remote-send error classification, player
// manager uid bindings, node_info/stats/setup optionals, server shutdown
// delay validation and watcher attachment, global data ttl/delta variants,
// rank batch-update filtering and around windows, scheduler pause/status,
// and the distributed-lock factory argument shapes.
// ---------------------------------------------------------------------------

#ifdef SHIELD_ENABLE_CLUSTER
// No cluster manager installed: shield.cluster.node_id() degrades to nil and
// a qualified target is treated as an ordinary local miss.
BOOST_AUTO_TEST_CASE(ClusterNodeIdNilWithoutGlobalManager) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);
    shield::cluster::set_global_cluster_manager(nullptr);

    BOOST_CHECK(run_script(
        lua,
        "assert(shield.cluster.node_id() == nil)\n"
        // A "node:service" target without a cluster namespace resolves as
        // a plain local miss (service_not_found), not a cluster error.
        "local ok, err = shield.send('cov2-b:echo', 'm', 1)\n"
        "assert(ok == false)\n"
        "assert(err.code == 'service_not_found', err.code)"));
}
#endif  // SHIELD_ENABLE_CLUSTER

#ifdef SHIELD_ENABLE_PLAYER
// The __shield_player_ref JSON marker materializes as a read-only ref with
// defaults for missing fields: string members ignore non-strings and the
// epoch only accepts non-negative integers.
BOOST_AUTO_TEST_CASE(PlayerRefMarkerDecodeDefaults) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::player::PlayerManager pm(shield::player::PlayerConfig{});
    shield::player::PlayerManager::set_global(&pm);

    // A session the decoded ref resolves against.
    shield::player::PlayerRef pr;
    pr.uid = "u1";
    pr.service_id = "svc_u1";
    pr.epoch = 7;
    pm.register_session(pr, "dev-1", shield::player::SessionState::kReady,
                        1000);

    // Full marker with an integer epoch resolves to the session.
    lua["cov2_ref_full"] = json_to_lua(
        shd::state_view(lua),
        nlohmann::json::parse(
            R"({"__shield_player_ref": true, "uid": "u1",)"
            R"("node_id": "", "service_id": "svc_u1", "epoch": 42})"));
    BOOST_CHECK(run_script(
        lua,
        "local info, err = shield.player.resolve(cov2_ref_full)\n"
        "assert(info ~= nil, err and (err.code .. ' ' .. err.message) or\n"
        "  'resolve returned nothing')"));

    // Sparse and malformed markers degrade: empty uid is rejected by
    // resolve with the stable code.
    for (const char* marker_json :
         {R"({"__shield_player_ref": true})",
          R"({"__shield_player_ref": true, "uid": 42})",
          R"({"__shield_player_ref": true, "epoch": -3})",
          R"({"__shield_player_ref": true, "epoch": 3.5})"}) {
        lua["cov2_ref_odd"] = json_to_lua(shd::state_view(lua),
                                          nlohmann::json::parse(marker_json));
        BOOST_CHECK(
            run_script(lua,
                       "local info, err = shield.player.resolve(cov2_ref_odd)\n"
                       "assert(info == nil)\n"
                       "assert(err.code == 'invalid_player_ref', err.code)"));
    }

    // Odd-typed node_id/service_id fields fail their is_string guards and
    // degrade to the empty default: the ref stays local and resolves.
    for (const char* marker_json :
         {R"({"__shield_player_ref": true, "uid": "u1", "node_id": 42})",
          R"({"__shield_player_ref": true, "uid": "u1", "service_id": 42})"}) {
        lua["cov2_ref_oddfield"] = json_to_lua(
            shd::state_view(lua), nlohmann::json::parse(marker_json));
        BOOST_CHECK(run_script(lua,
                               "local info, err = "
                               "shield.player.resolve(cov2_ref_oddfield)\n"
                               "assert(info ~= nil, err and err.code or\n"
                               "  'odd-field ref did not resolve')"));
    }

    shield::player::PlayerManager::set_global(nullptr);
}

// The shd-created PlayerRefBox (raw payload plus the uservalue type-name
// tag) serializes through the shd reader to the same marker JSON the sol
// reader produces.
BOOST_AUTO_TEST_CASE(PlayerRefBoxShdMarkerRoundTrip) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shd::object boxed = shd::make_object(
        shd::state_view(lua),
        PlayerRefBox{PlayerRefData{"u-shd", "dev-1", "svc-shd", 7}});
    const nlohmann::json expected{{"__shield_player_ref", true},
                                  {"uid", "u-shd"},
                                  {"node_id", "dev-1"},
                                  {"service_id", "svc-shd"},
                                  {"epoch", std::uint64_t{7}}};
    BOOST_CHECK(lua_to_json(boxed) == expected);

    // The lvalue box drives the const-ref push path (the rvalue forwarding
    // overload delegates through the same type-name registry), and the
    // unsigned boxes confirm the integral make_object entries on both the
    // lua_State* and state_view forms.
    PlayerRefBox boxed_lv{"u-lv", "dev-lv", "svc-lv", 3};
    lua_newtable(lua);
    shd::table t(lua, lua_gettop(lua));
    t.set("box", boxed_lv);
    shd::object n1 = shd::make_object(lua, std::uint64_t{42});
    shd::object n2 = shd::make_object(shd::state_view(lua), std::uint64_t{42});
    const nlohmann::json expected_lv{
        {"box", nlohmann::json{{"__shield_player_ref", true},
                               {"uid", "u-lv"},
                               {"node_id", "dev-lv"},
                               {"service_id", "svc-lv"},
                               {"epoch", std::uint64_t{3}}}}};
    BOOST_CHECK(lua_to_json(t) == expected_lv);
    BOOST_CHECK(n1.is<std::uint64_t>() &&
                n1.as<std::uint64_t>() == std::uint64_t{42});
    BOOST_CHECK(n2.is<std::uint64_t>() &&
                n2.as<std::uint64_t>() == std::uint64_t{42});
    lua_pop(lua, 1);
}

// register_session's table-ref path decodes the epoch from a decimal string
// (the canonical wire shape), accepts a plain integer, and degrades
// malformed epochs to zero; unknown-uid marks fail, and resolving a ref
// that names a foreign node reports the stable P0 code.
BOOST_AUTO_TEST_CASE(PlayerRegisterSessionEpochShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::player::PlayerManager pm(shield::player::PlayerConfig{});
    shield::player::PlayerManager::set_global(&pm);

    BOOST_CHECK(run_script(
        lua,
        "local M = shield.player.manager\n"
        // Decimal-string epoch (the stoull path).
        "M.register_session({uid = 'u1', service_id = 'svc_u1',\n"
        "  node_id = '', epoch = '42'}, 'dev-1', 'ready', 1000)\n"
        // Integer epoch (positive).
        "M.register_session({uid = 'u2', service_id = 'svc_u2',\n"
        "  epoch = 43}, 'dev-1', 'ready', 1000)\n"
        // Negative integer epoch: is<uint64_t>() accepts it (wrap-around to
        // 2^64-1), so the is<int> arm in player_ref_epoch never runs.
        "M.register_session({uid = 'u2n', service_id = 'svc_u2n',\n"
        "  epoch = -1}, 'dev-1', 'ready', 1000)\n"
        // Malformed string epoch degrades to zero; the session still lands.
        "M.register_session({uid = 'u3', service_id = 'svc_u3',\n"
        "  epoch = 'not-a-number'}, 'dev-1', 'ready', 1000)\n"
        // Fractional epoch hits neither numeric view.
        "M.register_session({uid = 'u4', service_id = 'svc_u4',\n"
        "  epoch = 3.5}, 'dev-1', 'ready', 1000)\n"
        // Missing epoch entirely.
        "M.register_session({uid = 'u5', service_id = 'svc_u5'}, 'dev-1',\n"
        "  'ready', 1000)\n"
        // Boolean epoch: neither numeric view accepts it, degrades to zero.
        "M.register_session({uid = 'u6', service_id = 'svc_u6',\n"
        "  epoch = true}, 'dev-1', 'ready', 1000)\n"
        "assert(M.size() == 7, M.size())\n"
        // Unknown-uid marks fail.
        "assert(M.mark_disconnected('ghost', 1100) == false)\n"
        "assert(M.mark_reconnected('ghost', 1100) == false)\n"
        // A ref naming a foreign node is rejected with the P0 code.
        "local info, err = shield.player.resolve({uid = 'u1',\n"
        "  node_id = 'far-node'})\n"
        "assert(info == nil)\n"
        "assert(err.code == 'remote_resolve_unimplemented', err.code)"));

    shield::player::PlayerManager::set_global(nullptr);
}

// node_info() without a player manager reports the empty locality; stats()
// reads the orchestration counters through the wrapper table; setup()
// without an options table runs the bare-module arm.
BOOST_AUTO_TEST_CASE(PlayerNodeInfoStatsAndSetupShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);
    shield::player::PlayerManager::set_global(nullptr);

    BOOST_CHECK(run_script(
        lua,
        "local ni = shield.player.node_info()\n"
        "assert(ni.node_id == '')\n"
        "assert(ni.epoch == '0')\n"
        "local st = shield.player.stats()\n"
        "assert(type(st) == 'table')\n"
        "assert(st.rejected_not_ready ~= nil)\n"
        "local r, err = shield.player.setup({})\n"
        "if r == nil then assert(err.code == 'setup_invalid', err.code) end"));
}
#endif  // SHIELD_ENABLE_PLAYER

#ifdef SHIELD_ENABLE_CLUSTER
// Remote send classification: a Suspect node fails resolution with the
// retryable node_suspect code, and a transport-seam failure carrying the
// suspect text maps through the same stable code.
BOOST_AUTO_TEST_CASE(ClusterRemoteSendSuspectAndSeamError) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shield::cluster::ClusterConfig cc;
    cc.enabled = true;
    cc.node_id = "cov2-a";
    cc.peers = {"127.0.0.1:39011"};
    cc.suspect_timeout_ms = 1;

    // Resolution-time failure: the peer handshake goes online, the suspect
    // window lapses, and tick() degrades the node to Suspect.
    shield::cluster::ClusterManager suspect_cm(cc);
    suspect_cm.start();
    suspect_cm.on_handshake("127.0.0.1:39011", "cov2-b", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    (void)suspect_cm.tick();
    BOOST_CHECK_EQUAL(suspect_cm.check_node_reachable("cov2-b"),
                      "node_suspect");
    shield::cluster::set_global_cluster_manager(&suspect_cm);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('cov2-b:echo', 'm', 1)\n"
                   "assert(ok == false)\n"
                   "assert(err.code == 'node_suspect', err.code)\n"
                   "assert(err.retryable == true)"));

    suspect_cm.stop();
    shield::cluster::set_global_cluster_manager(nullptr);

    // Seam-time failure: the node is online so resolution passes, but the
    // transport seam reports the suspect text, which maps to the same code.
    shield::cluster::ClusterManager seam_cm(cc);
    seam_cm.start();
    seam_cm.set_remote_send_fn([](const std::string&, const std::string&,
                                  const std::string&, const std::string&,
                                  uint64_t, int32_t, std::string* error) {
        if (error) *error = "node_suspect: transport seam down";
        return false;
    });
    seam_cm.on_handshake("127.0.0.1:39011", "cov2-b", 1);
    seam_cm.on_routes("cov2-b", 1, {{"echo", "svc_cov2_b"}});
    shield::cluster::set_global_cluster_manager(&seam_cm);

    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('cov2-b:echo', 'm', 1)\n"
                   "assert(ok == false)\n"
                   "assert(err.code == 'node_suspect', err.code)\n"
                   "assert(err.retryable == true)"));

    seam_cm.stop();
    shield::cluster::set_global_cluster_manager(nullptr);
}
#endif  // SHIELD_ENABLE_CLUSTER

#ifdef SHIELD_ENABLE_GLOBAL
// Global data optionals: the present and absent arms of the ttl and delta
// parameters across set/incr/decr/mset/get_cached, plus mset's non-string
// key skip.
BOOST_AUTO_TEST_CASE(GlobalDataTtlDeltaAndBatchArms) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::global::GlobalManager gm(shield::global::GlobalConfig{});
    shield::global::GlobalManager::set_global(&gm);

    BOOST_CHECK(run_script(lua,
                           "local g = assert(shield.global())\n"
                           "g:set('cov2_k1', 'v1', 250)\n"
                           "g:set('cov2_k2', 'v2')\n"
                           "assert(g:incr('cov2_c1') == 1)\n"
                           "g:incr('cov2_c1', 5)\n"
                           "g:decr('cov2_c1')\n"
                           "g:decr('cov2_c1', 3)\n"
                           "g:mset({cov2_a = '1', cov2_b = '2'}, 100)\n"
                           "g:mset({cov2_c = '3'})\n"
                           "g:mset({[9] = 'skipped', cov2_ok = 'y'})\n"
                           "assert(g:get('cov2_k1') == 'v1')\n"
                           "assert(g:get('cov2_ok') == 'y')\n"
                           "assert(g:get_cached('cov2_k2', 100) == 'v2')\n"
                           "assert(g:get_cached('cov2_k2') == 'v2')\n"));

    shield::global::GlobalManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_GLOBAL

#ifdef SHIELD_ENABLE_GLOBAL
// Rank batch updates skip malformed entries (non-string uid, non-number
// score); around() anchors on an existing uid and returns no target for
// an unknown one.
BOOST_AUTO_TEST_CASE(RankMupdateSkipsMalformedAndAroundWindow) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::global::GlobalManager gm(shield::global::GlobalConfig{});
    shield::global::GlobalManager::set_global(&gm);

    BOOST_CHECK(run_script(
        lua,
        "local b = assert(shield.rank('cov2_board'))\n"
        "assert(b:mupdate({u1 = 1.5, [2] = 9.9, u3 = 'x',\n"
        "  ['a-very-long-identifier-beyond-fifteen'] = 2.5}) == true)\n"
        "assert(b:count() == 2, b:count())\n"
        "local near = b:around('u1', 3)\n"
        "assert(near.target ~= nil)\n"
        "local miss = b:around('nobody-here', 3)\n"
        "assert(miss.target == nil)\n"));

    shield::global::GlobalManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_GLOBAL

#ifdef SHIELD_ENABLE_GLOBAL
// Scheduler bindings without a dispatch context: name validation rejects
// non-string and empty task names before anything else, a host-registered
// task pauses/resumes through the bindings, and get() reports the paused
// status.
BOOST_AUTO_TEST_CASE(SchedulerNameValidationPauseAndGet) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::global::GlobalManager gm(shield::global::GlobalConfig{});
    std::string sched_error;
    BOOST_REQUIRE(gm.sched_register("cron", "cov2_t1", "* * * * *", "svc_cov2",
                                    &sched_error));
    shield::global::GlobalManager::set_global(&gm);

    BOOST_CHECK(
        run_script(lua,
                   "local s = assert(shield.scheduler())\n"
                   "local ok1, err1 = s:cron(42, '* * * * *', function() end)\n"
                   "assert(ok1 == nil)\n"
                   "assert(err1.code == 'invalid_argument', err1.code)\n"
                   "local ok2, err2 = s:cron('', '* * * * *', function() end)\n"
                   "assert(ok2 == nil)\n"
                   "assert(err2.code == 'invalid_argument', err2.code)\n"
                   "assert(s:pause('cov2_t1') == true)\n"
                   "local info = assert(s:get('cov2_t1'))\n"
                   "assert(info.status == 'paused', info.status)\n"
                   "assert(s:resume('cov2_t1') == true)\n"
                   "assert(s:get('cov2_t1').status == 'active')\n"));

    shield::global::GlobalManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_GLOBAL

#ifdef SHIELD_ENABLE_GLOBAL
// The distributed-lock factory argument shapes: name only, name + options,
// a non-string name, and no name at all — every shape runs the name
// ternary arms without throwing.
BOOST_AUTO_TEST_CASE(DistributedMutexFactoryArgShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::global::GlobalManager gm(shield::global::GlobalConfig{});
    shield::global::GlobalManager::set_global(&gm);

    BOOST_CHECK(
        run_script(lua,
                   "local m1 = shield.distributed_mutex('cov2_m1')\n"
                   "assert(m1 ~= nil)\n"
                   "local m2 = shield.distributed_mutex('cov2_m2',\n"
                   "  {ttl_ms = 5000})\n"
                   "assert(m2 ~= nil)\n"
                   "assert(pcall(shield.distributed_mutex, 42,\n"
                   "  {ttl_ms = 100}))\n"
                   "assert(pcall(shield.distributed_mutex, nil))\n"
                   "local rw = shield.distributed_rwlock('cov2_rw',\n"
                   "  {ttl_ms = 100})\n"
                   "assert(rw ~= nil)\n"
                   // An opts table whose metatable errors on index makes
                   // the two-argument maker fail: invalid lock arguments.
                   "local bad = setmetatable({},\n"
                   "  {__index = function() error('opts boom') end})\n"
                   "local r, e = shield.distributed_mutex('cov2_m4', bad)\n"
                   "assert(r == nil)\n"
                   "assert(e.code == 'invalid_argument',\n"
                   "  e and e.code or 'no error')\n"));

    shield::global::GlobalManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_GLOBAL

#ifdef SHIELD_ENABLE_SERVER
// shield.server shutdown delay validation (above 2^53 rejected, a valid
// delay schedules the handover, a second schedule is refused) and unwatch
// of unknown ids through both numeric views.
BOOST_AUTO_TEST_CASE(ServerShutdownDelayAndUnwatchShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::server::ServerManager sm(shield::server::ServerConfig{});
    shield::server::ServerManager::set_global(&sm);

    BOOST_CHECK(run_script(
        lua,
        "local ok, err = shield.server.shutdown(10000000000000000)\n"
        "assert(ok == nil)\n"
        "assert(err.code == 'invalid_argument', err.code)\n"
        "local ok2, err2 = shield.server.shutdown(1500)\n"
        "assert(ok2 == true, err2 and err2.code)\n"
        "local ok3, err3 = shield.server.shutdown(1500)\n"
        "assert(ok3 == nil)\n"
        "assert(err3.code == 'shutdown_already_scheduled', err3.code)\n"
        "assert(shield.server.unwatch(77) == true)\n"
        "assert(shield.server.unwatch(88.0) == true)\n"
        // Non-numeric id: both numeric views miss, id stays 0.
        "assert(shield.server.unwatch('zzz') == true)\n"
        // Watch from the main thread lacks the dispatch context.
        "local w, werr = shield.server.watch(function() end)\n"
        "assert(w == nil)\n"
        "assert(werr.code == 'invalid_argument', werr.code)"));

    sm.stop();  // join the shutdown timer before the manager dies
    shield::server::ServerManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_SERVER

#ifdef SHIELD_ENABLE_SERVER
// A watcher registered from on_init (the spawning dispatch context)
// attaches through the runtime and lands in the server manager.
BOOST_AUTO_TEST_CASE(ServerWatchInsideOnInit) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shield::server::ServerManager sm(shield::server::ServerConfig{});
    shield::server::ServerManager::set_global(&sm);

    const std::string path =
        write_script("cov12_watcher.lua",
                     "local M = {}\n"
                     "local w_ok, w_err\n"
                     "function M.on_init()\n"
                     "  w_ok, w_err = shield.server.watch(function(state)\n"
                     "    return true\n"
                     "  end)\n"
                     "  return true\n"
                     "end\n"
                     "function M.get_watch(ctx)\n"
                     "  return w_ok, w_err and w_err.code or nil\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(path, opts_for("cov12_watcher").dump());
    BOOST_REQUIRE(svc.success);

    auto res =
        manager.call(svc.service_id, "get_watch", nlohmann::json::array());
    BOOST_REQUIRE(res.success);
    BOOST_REQUIRE(res.values.size() >= 2u);
    BOOST_CHECK(!res.values[0].is_null());  // a watch id came back
    BOOST_CHECK(res.values[1].is_null());   // no error
    BOOST_CHECK_EQUAL(sm.watcher_count(), 1u);

    manager.exit(svc.service_id, "done");
    sm.stop();
    shield::server::ServerManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_SERVER

#ifdef SHIELD_ENABLE_CLUSTER
// shield.cluster.node_id() reports the registered manager's id (both the
// short and the long heap-copy shapes), an empty id degrades to nil, and
// without a manager the binding returns nil.
BOOST_AUTO_TEST_CASE(ClusterNodeIdShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    // No global manager: nullopt arm.
    BOOST_CHECK(run_script(lua, "assert(shield.cluster.node_id() == nil)\n"));

    shield::cluster::ClusterConfig cc;
    cc.enabled = true;
    cc.node_id = "cov2-node-with-a-very-long-identifier-beyond-ssobound";
    shield::cluster::ClusterManager cm(cc);
    shield::cluster::set_global_cluster_manager(&cm);
    BOOST_CHECK(run_script(
        lua,
        "assert(shield.cluster.node_id() ==\n"
        "  'cov2-node-with-a-very-long-identifier-beyond-ssobound')\n"));

    // Live manager with an empty id: empty() arm of the guard.
    shield::cluster::ClusterConfig empty_cc;
    empty_cc.enabled = true;
    empty_cc.node_id = "";
    shield::cluster::ClusterManager empty_cm(empty_cc);
    shield::cluster::set_global_cluster_manager(&empty_cm);
    BOOST_CHECK(run_script(lua, "assert(shield.cluster.node_id() == nil)\n"));

    shield::cluster::set_global_cluster_manager(nullptr);
}
#endif  // SHIELD_ENABLE_CLUSTER

// ---------------------------------------------------------------------------
// Round-8 additions (branch coverage for remaining gaps):
// ---------------------------------------------------------------------------

#ifdef SHIELD_ENABLE_PLAYER
// json_to_lua: the __shield_player_ref marker's "== true" false arm.
// When the key is absent, value() returns false, so the == true check fails
// and the marker is treated as a plain table. This exercises the false arm.
BOOST_AUTO_TEST_CASE(PlayerRefMarkerFalseArm) {
    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);

    // Marker without __shield_player_ref key: falls through to plain table.
    const nlohmann::json plain = nlohmann::json::parse(R"({"uid": "u1"})");
    shd::object obj = json_to_lua(lua, plain);
    BOOST_CHECK(obj.is<shd::table>());
    shd::table t1 = obj.as<shd::table>();
    BOOST_CHECK(t1.get<shd::object>("uid").as<std::string>() == "u1");

    // Marker with __shield_player_ref = false (explicit false): false arm.
    const nlohmann::json explicit_false =
        nlohmann::json::parse(R"({"__shield_player_ref": false, "uid": "u1"})");
    shd::object obj2 = json_to_lua(lua, explicit_false);
    BOOST_CHECK(obj2.is<shd::table>());
    shd::table t2 = obj2.as<shd::table>();
    BOOST_CHECK(t2.get<shd::object>("uid").as<std::string>() == "u1");
    BOOST_CHECK(t2.get<shd::object>("__shield_player_ref").as<bool>() == false);
}

// json_to_lua: player-ref marker field is_string false arms (uid, node_id,
// service_id). Non-string values degrade to empty defaults; the ref still
// resolves locally if uid is valid.
BOOST_AUTO_TEST_CASE(PlayerRefMarkerFieldTypeCoercion) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::player::PlayerManager pm(shield::player::PlayerConfig{});
    shield::player::PlayerManager::set_global(&pm);

    // Seed a session with known uid.
    shield::player::PlayerRef pr;
    pr.uid = "u-coerce";
    pr.service_id = "svc-coerce";
    pr.epoch = 1;
    pm.register_session(pr, "dev-1", shield::player::SessionState::kReady,
                        1000);

    // node_id as number -> degrades to empty string; resolves locally.
    lua["ref_num_node"] = json_to_lua(
        shd::state_view(lua),
        nlohmann::json::parse(
            R"({"__shield_player_ref": true, "uid": "u-coerce", "node_id": 123})"));
    BOOST_CHECK(run_script(
        lua,
        "local info, err = shield.player.resolve(ref_num_node)\n"
        "assert(info ~= nil, err and err.code or 'resolve failed')"));

    // service_id as number -> degrades to empty string; resolves locally.
    lua["ref_num_svc"] = json_to_lua(
        shd::state_view(lua),
        nlohmann::json::parse(
            R"({"__shield_player_ref": true, "uid": "u-coerce", "service_id": 456})"));
    BOOST_CHECK(run_script(
        lua,
        "local info, err = shield.player.resolve(ref_num_svc)\n"
        "assert(info ~= nil, err and err.code or 'resolve failed')"));

    // uid as number -> degrades to empty string; resolve rejects with
    // invalid_player_ref.
    lua["ref_num_uid"] = json_to_lua(
        shd::state_view(lua),
        nlohmann::json::parse(
            R"({"__shield_player_ref": true, "uid": 789, "service_id": "svc"})"));
    BOOST_CHECK(
        run_script(lua,
                   "local info, err = shield.player.resolve(ref_num_uid)\n"
                   "assert(info == nil)\n"
                   "assert(err.code == 'invalid_player_ref', err.code)"));

    shield::player::PlayerManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_PLAYER

#ifdef SHIELD_ENABLE_PLAYER
// player_ref_epoch: the is<int>() false arm (non-integer numeric view,
// e.g., boolean or nil) degrades to 0. This is the catch-all after
// is<string>(), is<uint64_t>(), and is<int>() all fail.
BOOST_AUTO_TEST_CASE(PlayerRefEpochNonIntNumericDegradesToZero) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    shd::state lua;
    lua.open_libraries(shd::lib::base, shd::lib::coroutine, shd::lib::table,
                       shd::lib::string, shd::lib::os, shd::lib::math);
    register_full_shield_api(lua.lua_state(), &manager, &runtime);

    shield::player::PlayerManager pm(shield::player::PlayerConfig{});
    shield::player::PlayerManager::set_global(&pm);

    // Boolean epoch: neither is<uint64_t> nor is<int> accepts it -> 0.
    // Session still lands (epoch 0 is valid).
    BOOST_CHECK(run_script(lua,
                           "local M = shield.player.manager\n"
                           "M.register_session({uid = 'u-bool', service_id = "
                           "'svc', epoch = true},\n"
                           "  'dev-1', 'ready', 1000)\n"
                           "assert(M.size() == 1, M.size())"));

    // Nil epoch field (missing): all numeric views miss -> 0.
    BOOST_CHECK(run_script(
        lua,
        "local M = shield.player.manager\n"
        "M.register_session({uid = 'u-nil', service_id = 'svc'}, 'dev-1',\n"
        "  'ready', 1000)\n"
        "assert(M.size() == 2, M.size())"));

    shield::player::PlayerManager::set_global(nullptr);
}
#endif  // SHIELD_ENABLE_PLAYER

// shield.server.watch: the runtime-null guard arm. The binding is only
// invoked with a live runtime, so this arm is defensive and marked.
// We cannot manufacture a null-runtime call path; the marker stands as
// instruction-level evidence.

// shield.server.unwatch: the watch_id.is<double>() arm. Already exercised
// in ServerShutdownDelayAndUnwatchShapes with unwatch(88.0) -> double arm.
// No additional test needed.

// shield.global rank around: the around.target ternary arms. Already
// exercised in RankMupdateSkipsMalformedAndAroundWindow with both existing
// and unknown uids -> both arms covered. No additional test needed.

// shield.global register_task: the name guard compound arms. Already
// exercised in SchedulerNameValidationPauseAndGet with non-string (42) and
// empty string ('') -> both arms covered. No additional test needed.
