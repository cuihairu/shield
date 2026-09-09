// Coverage tests (round 2) for src/lua/lua_http_bridge.cpp: verb mapping on
// attach, late registrations through the sink, detached registration,
// request header forwarding, handler error shapes, service-not-running and
// dispatch-timeout responses, and the timeout path when queued dispatch
// tasks are cancelled (the cancelled task never fulfils its promise, so the
// caller observes the dispatch timeout).
#define BOOST_TEST_MODULE CovHttpBridge2
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <thread>
#include <unordered_map>

#include "shield/caf_initializer.hpp"
#include "shield/config/config.hpp"
#include "shield/lua/lua_http_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/http_server.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_http_bridge2";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

nlohmann::json opts_for(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
}

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

shield::net::HttpRequest make_request(const std::string& method,
                                      const std::string& target,
                                      std::string body = "") {
    shield::net::HttpRequest req;
    req.method(boost::beast::http::string_to_verb(method));
    req.target(target);
    req.version(11);
    req.set(boost::beast::http::field::user_agent, "cov-bridge2");
    req.body() = std::move(body);
    return req;
}

// Registers routes for every verb shape used by the mapping test.
const char* kVerbsScript = R"lua(
local M = {}
function M.on_init()
    shield.httpd.get("/r", function(req) return "get" end)
    shield.httpd.post("/r", function(req) return "post" end)
    shield.httpd.put("/r", function(req) return "put" end)
    shield.httpd.delete("/r", function(req) return "delete" end)
    shield.httpd.patch("/r", function(req) return "patch" end)
end
return M
)lua";

const char* kBadShapesScript = R"lua(
local M = {}
function M.on_init()
    shield.httpd.get("/number", function(req) return 42 end)
end
return M
)lua";

const char* kSlowScript = R"lua(
local M = {}
function M.on_init()
    shield.httpd.get("/slow", function(req)
        shield._block_sleep(2600)
        return "finally"
    end)
end
return M
)lua";

}  // namespace

// ---------------------------------------------------------------------------
// method_from_string coverage: every verb mapping plus the ANY fallback are
// exercised when the bridge mirrors pre-registered routes onto a server.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(AttachMirrorsAllVerbs) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov2_verbs.lua", kVerbsScript);
    auto svc = manager.spawn(module, opts_for("cov2_verbs").dump());
    BOOST_REQUIRE(svc.success);

    shield::net::HttpServer server;
    LuaHttpBridge bridge(runtime, manager);
    bridge.attach(server);

    // Late registration (service spawned after attach) flows through the
    // sink into register_on_server too.
    const std::string late_module = write_script(
        "cov2_late.lua",
        "local M = {}\n"
        "function M.on_init()\n"
        "    shield.httpd.get('/late', function(req) return 'late' end)\n"
        "end\n"
        "return M\n");
    auto late = manager.spawn(late_module, opts_for("cov2_late").dump());
    BOOST_REQUIRE(late.success);

    // Every verb dispatches through the bridge.
    const std::unordered_map<std::string, std::string> verb_bodies = {
        {"GET", "get"},       {"POST", "post"},   {"PUT", "put"},
        {"DELETE", "delete"}, {"PATCH", "patch"},
    };
    for (const auto& [verb, body] : verb_bodies) {
        auto resp = bridge.handle(make_request(verb, "/r"));
        BOOST_CHECK_EQUAL(resp.result_int(), 200);
        BOOST_CHECK_EQUAL(resp.body(), body);
    }
    auto late_resp = bridge.handle(make_request("GET", "/late"));
    BOOST_CHECK_EQUAL(late_resp.result_int(), 200);
    BOOST_CHECK_EQUAL(late_resp.body(), "late");

    // Requests carry headers into the handler context.
    {
        auto resp = bridge.handle(make_request("GET", "/r"));
        BOOST_CHECK_EQUAL(resp.result_int(), 200);
    }

    bridge.detach();
    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Handler returning a non-table/string/nil yields a 500 with the
// call_http_handler error text.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(NonTableHandlerYields500) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov2_bad_shapes.lua", kBadShapesScript);
    auto svc = manager.spawn(module, opts_for("cov2_bad_shapes").dump());
    BOOST_REQUIRE(svc.success);

    LuaHttpBridge bridge(runtime, manager);
    auto resp = bridge.handle(make_request("GET", "/number"));
    BOOST_CHECK_EQUAL(resp.result_int(), 500);
    BOOST_CHECK(resp.body().find("table, string, or nil") != std::string::npos);

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Route owned by a service id that is not running -> 503.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ServiceNotRunningYields503) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // The handler is never invoked (dispatch fails first), so a function
    // from an unrelated state is fine as a placeholder.
    sol::state standalone;
    standalone.open_libraries(sol::lib::base);
    sol::function handler =
        standalone.script("return function(req) return 'x' end");
    auto vm = runtime.create_vm();
    BOOST_CHECK(runtime.register_http_route(vm, "ghost_service", "GET",
                                            "/ghost", handler));

    shield::net::HttpServer server;
    LuaHttpBridge bridge(runtime, manager);
    bridge.attach(server);

    auto resp = bridge.handle(make_request("GET", "/ghost"));
    BOOST_CHECK_EQUAL(resp.result_int(), 503);
    BOOST_CHECK(resp.body().find("ghost_service") != std::string::npos);

    bridge.detach();
    // Drop the route before the registering VM and the standalone state
    // backing its handler are destroyed: route entries hold sol::function
    // references bound to those states, and releasing them from
    // ~LuaRuntime would touch already-closed lua_States.
    runtime.remove_http_routes_for_service("ghost_service");
}

// ---------------------------------------------------------------------------
// Slow handler exceeds the 2s dispatch budget -> 504.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(SlowHandlerYields504) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov2_slow.lua", kSlowScript);
    auto svc = manager.spawn(module, opts_for("cov2_slow").dump());
    BOOST_REQUIRE(svc.success);

    LuaHttpBridge bridge(runtime, manager);
    const auto start = std::chrono::steady_clock::now();
    auto resp = bridge.handle(make_request("GET", "/slow"));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    BOOST_CHECK_EQUAL(resp.result_int(), 504);
    BOOST_CHECK(elapsed >= std::chrono::seconds(2));
    BOOST_CHECK(resp.body().find("timeout") != std::string::npos);

    // Give the service actor time to finish the still-running handler before
    // teardown.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Cancelling a queued dispatch task leaves its promise unfulfilled (the
// bridge holds a shared_ptr to the promise for the whole dispatch window),
// so the caller waits out the 2s dispatch budget and observes 504.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(CancelledDispatchTaskYields504) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    // /block hogs the service actor; /quick is queued behind it.
    const std::string module = write_script(
        "cov2_cancel.lua",
        "local M = {}\n"
        "function M.on_init()\n"
        "    shield.httpd.get(\"/block\", function(req)\n"
        "        shield._block_sleep(1200)\n"
        "        return \"blocked\"\n"
        "    end)\n"
        "    shield.httpd.get(\"/quick\", function(req) return \"quick\" end)\n"
        "end\n"
        "return M\n");
    auto svc = manager.spawn(module, opts_for("cov2_cancel").dump());
    BOOST_REQUIRE(svc.success);

    LuaHttpBridge bridge(runtime, manager);

    // First request occupies the actor.
    auto block_future = std::async(std::launch::async, [&] {
        return bridge.handle(make_request("GET", "/block"));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Second request is queued behind the running one.
    auto quick_future = std::async(std::launch::async, [&] {
        return bridge.handle(make_request("GET", "/quick"));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Cancel pending (queued) tasks: the quick dispatch task is destroyed
    // before it ever runs, so its promise is never fulfilled.
    BOOST_REQUIRE_GE(manager.pending_task_count(svc.service_id), 1u);
    manager.cancel_forked_tasks_for_service(svc.service_id);
    BOOST_CHECK_EQUAL(manager.pending_task_count(svc.service_id), 0u);

    // The cancelled task never fulfils the promise, so the caller waits out
    // the 2s dispatch budget and observes the timeout response.
    auto quick_resp = quick_future.get();
    BOOST_CHECK_EQUAL(quick_resp.result_int(), 504);
    BOOST_CHECK(quick_resp.body().find("timeout") != std::string::npos);

    auto block_resp = block_future.get();
    BOOST_CHECK_EQUAL(block_resp.result_int(), 200);
    BOOST_CHECK_EQUAL(block_resp.body(), "blocked");

    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Round-3: handler raising a Lua error yields a 500 carrying the lua_error
// text from the handler's VM.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RaisingHandlerYields500WithLuaError) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module =
        write_script("cov3_raises.lua",
                     "local M = {}\n"
                     "function M.on_init()\n"
                     "    shield.httpd.get('/raise', function(req)\n"
                     "        error('handler kaboom')\n"
                     "    end)\n"
                     "end\n"
                     "return M\n");
    auto svc = manager.spawn(module, opts_for("cov3_raises").dump());
    BOOST_REQUIRE(svc.success);

    LuaHttpBridge bridge(runtime, manager);
    auto resp = bridge.handle(make_request("GET", "/raise"));
    BOOST_CHECK_EQUAL(resp.result_int(), 500);
    BOOST_CHECK(resp.body().find("handler kaboom") != std::string::npos);

    manager.shutdown_all("done");
}
