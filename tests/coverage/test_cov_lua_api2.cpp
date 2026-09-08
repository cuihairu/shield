// Coverage tests (round 2) for src/lua/lua_api.cpp: conversion helpers,
// main-thread sync call paths, httpd verb registration, plugin query APIs,
// deadline propagation, and SessionHandle branches on dead sessions.
#define BOOST_TEST_MODULE CovLuaApi2
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
  return ok, err and err.code or nil
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

    // Function values are not convertible; the direct form returns a null
    // marker ("<unsupported>" object) without failing.
    sol::object fn = lua.script("return function() end");
    nlohmann::json fn_json = lua_to_json(fn);
    BOOST_CHECK(fn_json.is_object());

    // Boolean member inside an array.
    sol::object arr = lua.script("return {true, false, nil, 's'}");
    nlohmann::json arr_json = lua_to_json(arr);
    BOOST_CHECK(arr_json.is_array());
    BOOST_CHECK_EQUAL(arr_json.size(), 4u);
    BOOST_CHECK(arr_json[0].get<bool>());
    BOOST_CHECK(!arr_json[1].get<bool>());
    BOOST_CHECK(arr_json[2].is_null());

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
                       sol::lib::string);
    register_full_shield_api(lua, &manager, &runtime);

    // Call to a service that does not exist.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield._sync_call_timeout("
                           "100, 'ghost', 'echo', 1)\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'service_not_found')"));

    // Call with a reserved method name is rejected before dispatch.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield._sync_call_timeout("
                           "100, 'ghost', 'on_reserved')\n"
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

    // Deadline: the caller uses call_timeout, so the callee handler observes
    // a positive remaining budget through shield.deadline().
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
        BOOST_CHECK(res.values[1].get<int64_t>() > 0);
    }

    // Calls / sends to an exited service map to service_dead.
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
        BOOST_CHECK(res.values[0].get<bool>());
    }
    {
        auto res =
            manager.call(caller.service_id, "get", nlohmann::json::array());
        BOOST_REQUIRE(res.success);
        BOOST_CHECK_EQUAL(res.values[2].get<std::string>(), "service_dead");
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
                       sol::lib::string);
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
// SessionHandle branches on a dead/unknown session id.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SessionHandleOnUnknownSession) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string);
    register_full_shield_api(lua, &manager, &runtime);

    BOOST_CHECK(run_script(lua,
                           "local s = __shield_make_session_handle("
                           "'999999', '1.2.3.4:5')\n"
                           "assert(s:id() == '999999')\n"
                           "assert(s:remote_addr() == '1.2.3.4:5')\n"
                           "local ok, err = s:send({k = 1})\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'session_closed')\n"
                           "local ok2, err2 = s:send('text')\n"
                           "assert(ok2 == false)\n"
                           "assert(err2.code == 'session_closed')\n"
                           "s:close('bye')\n"
                           "local bok, berr = s:bind_service('n', 'svc')\n"
                           "assert(bok == false)\n"
                           "assert(berr.code == 'session_closed')\n"
                           "local uok, uerr = s:unbind_service('n')\n"
                           "assert(uok == false)\n"
                           "assert(uerr.code == 'session_closed')\n"
                           "assert(s:get_service('n') == nil)\n"
                           "local pok, perr = s:set_player_id('p1')\n"
                           "assert(pok == false)\n"
                           "assert(perr.code == 'session_closed')\n"
                           "assert(s:player_id() == '')\n"
                           "assert(s:epoch() == 0)"));
}

// ---------------------------------------------------------------------------
// json_to_lua round-trip of a session-handle marker when the resolver
// registry holds a live session: make_session_handle_json populates the
// registry, json_to_lua rebuilds the userdata.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(MakeSessionHandleJsonPopulatesRegistry) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string);
    register_full_shield_api(lua, &manager, &runtime);

    // Fake session: id/remote are enough for handle creation.
    class FakeSession final : public shield::net::Session {
    public:
        shield::net::SessionId id() const override { return 4242; }
        shield::net::RemoteAddress remote_addr() const override {
            return shield::net::RemoteAddress{"127.0.0.1", 7777};
        }
        bool send(const std::vector<uint8_t>&,
                  std::string* = nullptr) override {
            return false;
        }
        void close(std::string) override {}
        bool is_alive() const override { return true; }
        std::string error_code() const override { return ""; }
        bool has_protocol_pipeline() const override { return false; }
        std::string_view protocol_codec_name() const override { return {}; }
        bool send_message(const shield::transport::DecodedBody&,
                          std::string*) override {
            return false;
        }
        void set_user_data(std::string, std::string) override {}
        std::string get_user_data(std::string_view) const override {
            return "";
        }
        void set_target_service(std::string) override {}
        std::string target_service() const override { return ""; }
        void set_player_id(std::string) override {}
        std::string player_id() const override { return ""; }
        void set_epoch(uint32_t) override {}
        uint32_t epoch() const override { return 0; }
        shield::net::SessionRoutingContext& routing_context() override {
            return ctx_;
        }
        const shield::net::SessionRoutingContext& routing_context()
            const override {
            return ctx_;
        }
        void set_protocol_profile_id(std::string) override {}
        std::string protocol_profile_id() const override { return ""; }
        void unbind_service(const std::string&) override {}
        void bind_service(const std::string&,
                          shield::net::ServiceAddress) override {}
        const shield::net::ServiceAddress* get_service(
            const std::string&) const override {
            return nullptr;
        }

    private:
        shield::net::SessionRoutingContext ctx_;
    };

    auto session = std::make_shared<FakeSession>();
    nlohmann::json marker = make_session_handle_json(session);
    BOOST_CHECK(marker.is_object());
    BOOST_CHECK(marker.value("__shield_session_handle", false));

    lua["h"] = json_to_lua(sol::state_view(lua), marker);
    BOOST_CHECK(run_script(lua,
                           "assert(h:id() == tostring(4242))\n"
                           "assert(h:remote_addr():find('7777'))"));
}
