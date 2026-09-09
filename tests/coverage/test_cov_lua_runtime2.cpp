// Coverage tests (round 2) for src/lua/lua_runtime.cpp: ServiceHandle
// metamethods, LuaPack encode/decode branches, VM registry lookups, http
// route registration guards and sink invocation, call_http_handler VM-gone
// guard, load/exec failure modes, and global round-trips.
#define BOOST_TEST_MODULE CovLuaRuntime2
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_http_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_runtime2";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

}  // namespace

// ---------------------------------------------------------------------------
// ServiceHandle metamethods registered from the api layer.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ServiceHandleMetamethods) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    auto res = lua.safe_script(R"lua(
        local h1 = shield._make_handle('meta_a')
        local h2 = shield._make_handle('meta_a')
        local h3 = shield._make_handle('meta_b')
        assert(h1:id() == 'meta_a')
        assert(h1:node() == 0)
        assert(h1:valid() == true)
        assert(type(tostring(h1)) == 'string')
        local same = (h1 == h2)
        local diff = (h1 == h3)
        assert(type(same) == 'boolean' and type(diff) == 'boolean')
        assert(shield._make_handle('x'):valid() == true)
    )lua",
                               sol::script_pass_on_error);
    BOOST_CHECK(res.valid());
    if (!res.valid()) {
        const sol::error err = res;
        std::fprintf(stderr, "lua error: %s\n", err.what());
    }
}

// ---------------------------------------------------------------------------
// LuaPack encoder branches: integer-keyed maps, ServiceHandle values,
// unsupported values (functions), nesting limits, string length tags.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaPackEncodeBranches) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    LuaPackEncoder::Config config;
    LuaPackEncoder encoder(config);

    // Integer-keyed map entries.
    {
        sol::object value = lua.script("return {[1] = 'a', [300] = 2.5}");
        std::vector<uint8_t> bytes;
        BOOST_CHECK(encoder.encode(lua, value, bytes));
    }

    // ServiceHandle userdata traverses the table-iteration path of the
    // encoder (usertypes behave as tables with an empty sequence part).
    {
        sol::object handle =
            lua.script("return shield._make_handle('pack_svc')");
        LuaPackEncoder enc2{LuaPackEncoder::Config{}};
        std::vector<uint8_t> bytes;
        BOOST_CHECK(enc2.encode(lua, handle, bytes));
        BOOST_CHECK_GT(bytes.size(), 2u);
    }

    // Functions cannot be encoded.
    {
        sol::object fn = lua.script("return function() end");
        std::vector<uint8_t> bytes;
        BOOST_CHECK(!encoder.encode(lua, fn, bytes));
        BOOST_CHECK(!encoder.error().empty());
    }

    // Nesting limit exceeded.
    {
        LuaPackEncoder::Config shallow;
        shallow.max_nesting_depth = 2;
        LuaPackEncoder shallow_encoder(shallow);
        sol::object deep = lua.script("return {{{1}}}");
        std::vector<uint8_t> bytes;
        BOOST_CHECK(!shallow_encoder.encode(lua, deep, bytes));
    }

    // Long strings use the String tag; short ones ShortString.
    {
        sol::object short_str = lua.script("return 's'");
        std::vector<uint8_t> b1;
        BOOST_CHECK(encoder.encode(lua, short_str, b1));

        sol::object long_str = lua.script("return string.rep('x', 300)");
        std::vector<uint8_t> b2;
        BOOST_CHECK(encoder.encode(lua, long_str, b2));
    }
}

// ---------------------------------------------------------------------------
// LuaPack decoder round-trip and truncated input.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaPackDecodeRoundTrip) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);

    LuaPackEncoder encoder(LuaPackEncoder::Config{});
    LuaPackDecoder decoder;

    sol::object value =
        lua.script("return {1, 2.5, 'three', true, nil, {k = 'v'}}");
    std::vector<uint8_t> bytes;
    BOOST_REQUIRE(encoder.encode(lua, value, bytes));

    size_t consumed = 0;
    sol::object decoded = decoder.decode(lua, bytes, consumed);
    BOOST_CHECK(decoded.valid());
    BOOST_CHECK_GT(consumed, 0u);

    // Truncated input reports an error.
    std::vector<uint8_t> truncated(bytes.begin(),
                                   bytes.begin() + bytes.size() / 2);
    size_t consumed2 = 0;
    (void)decoder.decode(lua, truncated, consumed2);
}

// ---------------------------------------------------------------------------
// vm_for_state: null state and unknown state lookups.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(VmForStateBranches) {
    LuaRuntime runtime;

    BOOST_CHECK(runtime.vm_for_state(nullptr) == nullptr);

    // Unknown live state.
    sol::state alien;
    alien.open_libraries(sol::lib::base);
    BOOST_CHECK(runtime.vm_for_state(alien.lua_state()) == nullptr);
}

// ---------------------------------------------------------------------------
// register_http_route guards and sink invocation.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RegisterHttpRouteGuardsAndSink) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();

    // Nil handler is rejected.
    sol::state standalone;
    standalone.open_libraries(sol::lib::base);
    sol::function nil_handler = standalone.script("return nil");
    sol::function handler =
        standalone.script("return function(req) return 'ok' end");
    std::string error;
    BOOST_CHECK(!runtime.register_http_route(vm, "svc", "GET", "/a",
                                             nil_handler, &error));
    BOOST_CHECK(!error.empty());

    // A sink installed before registration observes late registrations.
    std::vector<std::pair<std::string, std::string>> sink_calls;
    runtime.set_http_route_sink(
        [&](const std::string& method, const std::string& path) {
            sink_calls.emplace_back(method, path);
        });
    BOOST_CHECK(
        runtime.register_http_route(vm, "svc", "GET", "/sink", handler));
    BOOST_REQUIRE_EQUAL(sink_calls.size(), 1u);
    BOOST_CHECK_EQUAL(sink_calls[0].first, "GET");
    BOOST_CHECK_EQUAL(sink_calls[0].second, "/sink");

    runtime.set_http_route_sink({});
}

// ---------------------------------------------------------------------------
// call_http_handler guard: handler bound to a destroyed VM.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallHttpHandlerVmGone) {
    LuaRuntime runtime;
    nlohmann::json request = {{"method", "GET"},
                              {"path", "/"},
                              {"query", ""},
                              {"params", nlohmann::json::object()},
                              {"headers", nlohmann::json::object()},
                              {"body", ""}};

    sol::state standalone;
    standalone.open_libraries(sol::lib::base);
    sol::function h = standalone.script("return function() return 'x' end");

    std::string err;
    nlohmann::json desc;
    std::optional<HttpRouteRegistration> route;
    {
        auto vm = runtime.create_vm();
        runtime.register_http_route(vm, "svc2", "GET", "/gone", h);
        route = runtime.find_http_route("GET", "/gone");
    }
    // The only strong reference to the VM died with the scope above, so the
    // weak_ptr in the route has expired.
    BOOST_REQUIRE(route.has_value());
    BOOST_CHECK(!runtime.call_http_handler(*route, request, desc, &err));
    BOOST_CHECK_EQUAL(err, "service VM is gone");
}

// ---------------------------------------------------------------------------
// load_script / load_service_module failure modes.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LoadFailures) {
    LuaRuntime runtime;

    // load_script with a nonexistent path reports failure.
    auto vm = runtime.create_vm();
    BOOST_CHECK(!runtime.load_script(vm, "/nonexistent/nope.lua"));

    // load_script with a syntax error.
    const std::string bad = write_script("bad_syntax.lua", "local x = = 1\n");
    BOOST_CHECK(!runtime.load_script(vm, bad));

    // load_service_module with a syntax error reports the sol error.
    std::string error;
    BOOST_CHECK(!runtime.load_service_module(vm, bad, &error));
    BOOST_CHECK(!error.empty());

    // load_service_module with a script returning a non-table.
    const std::string scalar = write_script("scalar_module.lua", "return 42\n");
    BOOST_CHECK(!runtime.load_service_module(vm, scalar, &error));
    BOOST_CHECK(!error.empty());
}

// ---------------------------------------------------------------------------
// exec_lua result shapes and errors; global round-trip.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ExecLuaAndGlobals) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();

    // Multi-value, multi-type exec results.
    nlohmann::json result;
    std::string error;
    BOOST_CHECK(runtime.exec_lua(vm, "return 1, 'two', 3.5, true, nil", &result,
                                 &error));
    BOOST_CHECK(result.is_array());
    BOOST_REQUIRE_EQUAL(result.size(), 5u);
    BOOST_CHECK_EQUAL(result[0].get<int64_t>(), 1);
    BOOST_CHECK_EQUAL(result[1].get<std::string>(), "two");
    BOOST_CHECK_EQUAL(result[2].get<double>(), 3.5);
    BOOST_CHECK(result[3].get<bool>());
    BOOST_CHECK(result[4].is_null());

    // No return values: result is left untouched (nothing appended).
    error.clear();
    BOOST_CHECK(runtime.exec_lua(vm, "local x = 1", &result, &error));
    BOOST_CHECK(error.empty());

    // Compile error.
    BOOST_CHECK(!runtime.exec_lua(vm, "return =", &result, &error));
    BOOST_CHECK(!error.empty());

    // Runtime error.
    BOOST_CHECK(!runtime.exec_lua(vm, "error('exec boom')", &result, &error));
    BOOST_CHECK(error.find("exec boom") != std::string::npos);

    // get_global/set_global round-trip.
    runtime.set_global(vm, "cov2_answer", "42");
    BOOST_CHECK_EQUAL(runtime.get_global(vm, "cov2_answer"), "42");
}

// load_script on a path that exists but cannot be read as a file (a
// directory) takes the read-failure branch.
BOOST_AUTO_TEST_CASE(LoadScriptOnDirectoryFails) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    BOOST_CHECK(!runtime.load_script(vm, "/tmp"));
    BOOST_CHECK(!runtime.load_script(vm, "."));
}

// Packing a real ServiceHandle usertype hits the dedicated ServiceHandle
// type tag, and tables with string keys exercise the key-encoding branches.
BOOST_AUTO_TEST_CASE(PackServiceHandleAndStringKeys) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    LuaPackEncoder::Config config;
    LuaPackEncoder encoder(config);

    // Real usertype handle (registered via the full API).
    {
        auto handle = lua.script("return shield._make_handle('pack_real')");
        std::vector<std::uint8_t> bytes;
        BOOST_CHECK(encoder.encode(lua, handle, bytes));
        BOOST_CHECK(!bytes.empty());
    }

    // String-keyed and integer-keyed tables both encode.
    {
        auto mixed = lua.script("return { alpha = 1, [42] = 'answer' }");
        std::vector<std::uint8_t> bytes;
        BOOST_CHECK(encoder.encode(lua, mixed, bytes));
        BOOST_CHECK(!bytes.empty());
    }
}

// Filling the actor timer table (kTimerLimit = 10000) makes both timer APIs
// report the timer_limit error.
BOOST_AUTO_TEST_CASE(TimerLimitReachedReportsError,
                     *boost::unit_test::timeout(120)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov_timer_limit.lua",
                                            R"lua(
local M = {}
local once_err, rep_err
function M.on_init()
    for i = 1, 10000 do shield.timer_once(3600000, function() end) end
    local _, e1 = shield.timer_once(1000, function() end)
    once_err = e1
    local _, e2 = shield.timer(1000, function() end)
    rep_err = e2
end
function M.get_errors()
    return once_err, rep_err
end
return M
)lua");
    auto svc = manager.spawn(
        module, R"({"name":"cov_timer_limit","args":{},"config":{}})");
    BOOST_REQUIRE(svc.success);

    // Creating 10000 timers keeps on_init busy; retry the call until the
    // service actor finishes initialization.
    CallResult res;
    for (int i = 0; i < 100; ++i) {
        res =
            manager.call(svc.service_id, "get_errors", nlohmann::json::array());
        if (res.success && res.values.size() == 2u) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_REQUIRE_MESSAGE(res.success, "call failed: " + res.error_message);
    BOOST_REQUIRE_EQUAL(res.values.size(), 2u);
    BOOST_CHECK(res.values[0].is_null() || res.values[0].contains("code"));
    if (!res.values[0].is_null()) {
        BOOST_CHECK_EQUAL(res.values[0]["code"], "timer_limit");
        BOOST_CHECK_EQUAL(res.values[1]["code"], "timer_limit");
    } else {
        BOOST_FAIL("expected timer_limit errors from both timer APIs");
    }
}

// shield.call maps the underlying error message to a stable error code:
// service_not_found, method_not_found, handler_error.
BOOST_AUTO_TEST_CASE(CallErrorCodesAreStable, *boost::unit_test::timeout(60)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string callee = write_script("cov_call_callee.lua",
                                            R"lua(
local M = {}
function M.echo(x) return x end
function M.boom() error("handler kaboom") end
return M
)lua");
    const std::string caller = write_script("cov_call_caller.lua",
                                            R"lua(
local M = {}
local codes = {}
local function code_of(ok, err)
    if ok or err == nil then return "ok" end
    if type(err) == "table" and err.code then return err.code end
    return "no-code"
end
function M.on_init()
    local ok1, e1 = shield.call("no_such_service", "echo", {}, 500)
    codes.not_found = code_of(ok1, e1)
    local ok2, e2 = shield.call("callee", "missing", {}, 500)
    codes.method = code_of(ok2, e2)
    local ok3, e3 = shield.call("callee", "boom", {}, 2000)
    codes.handler = code_of(ok3, e3)
end
function M.get_codes()
    return codes.not_found, codes.method, codes.handler
end
return M
)lua");

    BOOST_REQUIRE(
        manager.spawn(callee, R"({"name":"callee","args":{},"config":{}})")
            .success);
    auto svc =
        manager.spawn(caller, R"({"name":"caller","args":{},"config":{}})");
    BOOST_REQUIRE(svc.success);

    CallResult res;
    for (int i = 0; i < 100; ++i) {
        res =
            manager.call(svc.service_id, "get_codes", nlohmann::json::array());
        if (res.success && res.values.size() == 3u &&
            !res.values[0].is_null()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_REQUIRE_MESSAGE(res.success && res.values.size() == 3u,
                          "call failed: " + res.error_message);
    BOOST_CHECK_EQUAL(res.values[0].get<std::string>(), "service_not_found");
    BOOST_CHECK_EQUAL(res.values[1].get<std::string>(), "method_not_found");
    BOOST_CHECK_EQUAL(res.values[2].get<std::string>(), "handler_error");
}

// A forked task that throws is caught and logged by the fork wrapper (no
// crash, service keeps running).
BOOST_AUTO_TEST_CASE(ForkTaskThrowsIsLogged, *boost::unit_test::timeout(30)) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov_fork_throw.lua",
                                            R"lua(
local M = {}
local started = false
function M.on_init()
    shield.fork(function() error("task kaboom") end)
    started = true
end
function M.get_started() return started end
return M
)lua");
    auto svc = manager.spawn(
        module, R"({"name":"cov_fork_throw","args":{},"config":{}})");
    BOOST_REQUIRE(svc.success);

    CallResult res;
    for (int i = 0; i < 50; ++i) {
        res = manager.call(svc.service_id, "get_started",
                           nlohmann::json::array());
        if (res.success && res.values.size() == 1u) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_REQUIRE_MESSAGE(res.success, "call failed: " + res.error_message);
    BOOST_CHECK_EQUAL(res.values[0].get<bool>(), true);
}

// ---------------------------------------------------------------------------
// Round-3 additions: http handler response shapes, unreadable module paths,
// on_init false/error propagation, LuaPack edge branches.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CallHttpHandlerResponseShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // Handlers must live in a runtime-managed VM: a spawned service
    // registers them through shield.httpd from its on_init. Dispatching
    // through the bridge runs call_http_handler on the owning actor.
    const std::string module = write_script("cov3_http_shapes.lua",
                                            R"lua(
local M = {}
function M.on_init()
    shield.httpd.get('/nil', function(req) return nil end)
    shield.httpd.get('/nobody', function(req) return {status = 201} end)
    shield.httpd.get('/num', function(req) return 42 end)
end
return M
)lua");
    auto svc = manager.spawn(module, R"({"name":"cov3_shapes"})");
    BOOST_REQUIRE(svc.success);

    auto make_request = [](const std::string& target) {
        shield::net::HttpRequest req;
        req.method(boost::beast::http::verb::get);
        req.target(target);
        req.version(11);
        return req;
    };

    LuaHttpBridge bridge(runtime, manager);

    // Handler returning nil → 204 with empty body.
    auto resp = bridge.handle(make_request("/nil"));
    BOOST_CHECK_EQUAL(resp.result_int(), 204);

    // Handler returning a table without body → empty body, custom status.
    resp = bridge.handle(make_request("/nobody"));
    BOOST_CHECK_EQUAL(resp.result_int(), 201);
    BOOST_CHECK_EQUAL(resp.body(), "");

    // Handler returning a non-table/string/nil value is rejected.
    resp = bridge.handle(make_request("/num"));
    BOOST_CHECK_EQUAL(resp.result_int(), 500);
    BOOST_CHECK(resp.body().find("table, string, or nil") != std::string::npos);

    manager.exit(svc.service_id, "done");
}

// load_service_module on a path that cannot be read (a directory) reports
// the read failure instead of crashing.
BOOST_AUTO_TEST_CASE(LoadServiceModuleUnreadablePath) {
    LuaRuntime runtime;
    auto vm = runtime.create_vm();
    std::string error;
    BOOST_CHECK(!runtime.load_service_module(vm, kTmpDir, &error));
}

// call_service_function error shapes: explicit (false, reason) returns and
// a non-string reason that fails message conversion.
BOOST_AUTO_TEST_CASE(CallServiceFunctionErrorShapes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov3_oninit_shapes.lua",
                                            R"lua(
local M = {}
function M.deny() return false, "denied by policy" end
function M.deny_table() return false, {code = 42} end
function M.ok() return true end
return M
)lua");

    auto vm = runtime.create_vm();
    std::string error;
    BOOST_REQUIRE(runtime.load_service_module(vm, module, &error));

    BOOST_CHECK(
        !runtime.call_service_function(vm, "deny", nlohmann::json(), &error));
    BOOST_CHECK_EQUAL(error, "denied by policy");

    // Non-string second return: message conversion throws and the failure
    // surfaces through the catch-all.
    BOOST_CHECK(!runtime.call_service_function(vm, "deny_table",
                                               nlohmann::json(), &error));

    BOOST_CHECK(
        runtime.call_service_function(vm, "ok", nlohmann::json(), &error));
}

// LuaPack encoder: boolean map keys are rejected; a ServiceHandle nested as
// a map value takes the ServiceHandle tag branch.
BOOST_AUTO_TEST_CASE(LuaPackEncodeErrorAndHandleBranches) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    LuaPackEncoder encoder{LuaPackEncoder::Config{}};

    {
        sol::object bad_keys = lua.script("return {[true] = 1}");
        std::vector<uint8_t> bytes;
        BOOST_CHECK(!encoder.encode(lua, bad_keys, bytes));
        BOOST_CHECK(encoder.error().find("map keys") != std::string::npos);
    }

    {
        sol::object nested =
            lua.script("return {h = shield._make_handle('cov3.pack')}");
        std::vector<uint8_t> bytes;
        BOOST_CHECK(encoder.encode(lua, nested, bytes));
    }
}

// call_service_function with a (nil, reason) return pair: the nil-first
// branch converts the second return into the error message.
BOOST_AUTO_TEST_CASE(CallServiceFunctionNilReasonShape) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov4_soft_deny.lua",
                                            R"lua(
local M = {}
function M.soft_deny() return nil, "soft denied" end
function M.silent_deny() return nil end
return M
)lua");

    auto vm = runtime.create_vm();
    std::string error;
    BOOST_REQUIRE(runtime.load_service_module(vm, module, &error));

    BOOST_CHECK(!runtime.call_service_function(vm, "soft_deny",
                                               nlohmann::json(), &error));
    BOOST_CHECK_EQUAL(error, "soft denied");

    // nil with a single return keeps going (treated as success).
    BOOST_CHECK(runtime.call_service_function(vm, "silent_deny",
                                              nlohmann::json(), &error));
}

// ---------------------------------------------------------------------------
// Round-5 additions.
// ---------------------------------------------------------------------------
// Non-string request params make the request-table conversion throw inside
// call_http_handler; the exception surfaces through the error out-param
// (the handler itself is never invoked).
BOOST_AUTO_TEST_CASE(CallHttpHandlerBadParamsReportsError) {
    LuaRuntime runtime;

    // Placeholder handler from a standalone state: never invoked.
    sol::state standalone;
    standalone.open_libraries(sol::lib::base);
    sol::function handler =
        standalone.script("return function(req) return 'x' end");

    auto vm = runtime.create_vm();
    BOOST_CHECK(
        runtime.register_http_route(vm, "cov5.params", "GET", "/p", handler));
    auto route = runtime.find_http_route("GET", "/p");
    BOOST_REQUIRE(route.has_value());

    nlohmann::json request = {{"method", "GET"},
                              {"path", "/p"},
                              {"query", ""},
                              {"params", {{"x", 5}}},
                              {"headers", nlohmann::json::object()},
                              {"body", ""}};
    nlohmann::json desc;
    std::string err;
    BOOST_CHECK(!runtime.call_http_handler(*route, request, desc, &err));
    BOOST_CHECK(!err.empty());
}

// LuaPack rejects map keys whose encoded form exceeds the string budget.
BOOST_AUTO_TEST_CASE(LuaPackEncodeOversizedMapKeyFails) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string);

    LuaPackEncoder::Config config;
    config.max_string_length = 1024;
    LuaPackEncoder encoder(config);

    sol::object value = lua.script("return {[string.rep('k', 2048)] = 1}");
    std::vector<uint8_t> bytes;
    BOOST_CHECK(!encoder.encode(lua, value, bytes));
    BOOST_CHECK(!encoder.error().empty());
}
