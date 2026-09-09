// Coverage tests for shield.httpd route registration (LuaRuntime table +
// LuaHttpBridge dispatch onto service actor threads).
#define BOOST_TEST_MODULE CovLuaHttpBridge
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
#include "shield/config/config.hpp"
#include "shield/lua/lua_http_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_lua_http_bridge";

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

shield::net::HttpRequest make_request(const std::string& method,
                                      const std::string& target,
                                      std::string body = "") {
    shield::net::HttpRequest req;
    req.method(boost::beast::http::string_to_verb(method));
    req.target(target);
    req.version(11);
    req.body() = std::move(body);
    return req;
}

// Service registering one route per response shape.
const char* kHttpdScript = R"lua(
local M = {}

function M.on_init()
    shield.httpd.get("/health", function(req)
        return { status = 200, body = { ok = true } }
    end)
    shield.httpd.get("/echo/:id", function(req)
        return {
            status = 201,
            headers = { ["X-Param"] = req.params.id },
            body = "id=" .. req.params.id .. " query=" .. req.query ..
                   " method=" .. req.method,
        }
    end)
    shield.httpd.post("/data", function(req)
        return { status = 200, body = req.body }
    end)
    shield.httpd.get("/plain", function(req)
        return "raw string body"
    end)
    shield.httpd.get("/boom", function(req)
        error("handler exploded")
    end)
end

return M
)lua";

}  // namespace

// ---------------------------------------------------------------------------
// LuaRuntime HTTP route table.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RouteTableRegistrationAndLookup) {
    BOOST_TEST_CHECKPOINT("constructing runtime");
    LuaRuntime runtime;
    BOOST_TEST_CHECKPOINT("creating vm");
    auto vm = runtime.create_vm();

    // Table-management test only: the handler is never invoked, so a
    // function from an unrelated state suffices as a non-nil callable.
    // (safe_script: the unprotected script() path has misbehaved on
    // macOS/Release runners.)
    BOOST_TEST_CHECKPOINT("creating standalone state");
    sol::state standalone;
    standalone.open_libraries(sol::lib::base);
    BOOST_TEST_CHECKPOINT("compiling handler");
    sol::function handler = standalone.safe_script("return function() end",
                                                   sol::script_pass_on_error);
    BOOST_REQUIRE(handler.valid());
    BOOST_TEST_CHECKPOINT("handler ready");

    std::string error;
    // Invalid registrations.
    BOOST_CHECK(
        !runtime.register_http_route(vm, "", "GET", "/a", handler, &error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!runtime.register_http_route(nullptr, "svc", "GET", "/a",
                                             handler, &error));
    BOOST_CHECK(!runtime.register_http_route(vm, "svc", "GET", "no-slash",
                                             handler, &error));

    // Valid registrations.
    BOOST_CHECK(runtime.register_http_route(vm, "svc_a", "GET", "/a", handler));
    BOOST_CHECK(
        runtime.register_http_route(vm, "svc_a", "GET", "/users/:id", handler));
    BOOST_CHECK_EQUAL(runtime.http_route_count(), 2u);

    // Same method+path replaces the earlier registration.
    BOOST_CHECK(runtime.register_http_route(vm, "svc_b", "GET", "/a", handler));
    BOOST_CHECK_EQUAL(runtime.http_route_count(), 2u);
    auto replaced = runtime.find_http_route("GET", "/a");
    BOOST_REQUIRE(replaced);
    BOOST_CHECK_EQUAL(replaced->service_id, "svc_b");

    // Parameter pattern lookup captures params.
    std::vector<std::pair<std::string, std::string>> params;
    auto found = runtime.find_http_route("GET", "/users/42", &params);
    BOOST_REQUIRE(found);
    BOOST_CHECK_EQUAL(found->service_id, "svc_a");
    BOOST_REQUIRE_EQUAL(params.size(), 1u);
    BOOST_CHECK_EQUAL(params[0].first, "id");
    BOOST_CHECK_EQUAL(params[0].second, "42");

    // Method mismatch and segment-count mismatch do not match.
    BOOST_CHECK(!runtime.find_http_route("POST", "/a"));
    BOOST_CHECK(!runtime.find_http_route("GET", "/users/42/orders"));

    // Service-scoped removal.
    runtime.remove_http_routes_for_service("svc_a");
    BOOST_CHECK_EQUAL(runtime.http_route_count(), 1u);
    BOOST_CHECK(runtime.find_http_route("GET", "/a"));
}

// ---------------------------------------------------------------------------
// End-to-end: service registers routes; bridge dispatches a request through
// the owning service's actor thread.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(BridgeDispatchesToServiceHandler) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov_httpd_svc.lua", kHttpdScript);
    auto svc = manager.spawn(
        module, R"({"name":"cov_httpd_svc","args":{},"config":{}})");
    BOOST_REQUIRE_MESSAGE(svc.success, "spawn failed: " + svc.error_message);
    BOOST_REQUIRE_EQUAL(runtime.http_route_count(), 5u);

    shield::net::HttpServer server;
    LuaHttpBridge bridge(runtime, manager);
    bridge.attach(server);

    // JSON body response.
    {
        auto resp = bridge.handle(make_request("GET", "/health"));
        BOOST_CHECK_EQUAL(resp.result_int(), 200);
        BOOST_CHECK(resp.body().find("\"ok\":true") != std::string::npos);
        BOOST_CHECK_EQUAL(
            std::string(resp[boost::beast::http::field::content_type]),
            "application/json");
    }

    // Path parameter + query string + custom header + custom status.
    {
        auto resp = bridge.handle(make_request("GET", "/echo/42?q=1"));
        BOOST_CHECK_EQUAL(resp.result_int(), 201);
        BOOST_CHECK_EQUAL(std::string(resp.base()["X-Param"]), "42");
        BOOST_CHECK(resp.body().find("id=42") != std::string::npos);
        BOOST_CHECK(resp.body().find("query=q=1") != std::string::npos);
        BOOST_CHECK(resp.body().find("method=GET") != std::string::npos);
    }

    // POST body round-trip as a raw string body.
    {
        auto resp = bridge.handle(make_request("POST", "/data", "payload"));
        BOOST_CHECK_EQUAL(resp.result_int(), 200);
        BOOST_CHECK_EQUAL(resp.body(), "payload");
    }

    // Plain string return becomes a text/plain body.
    {
        auto resp = bridge.handle(make_request("GET", "/plain"));
        BOOST_CHECK_EQUAL(resp.result_int(), 200);
        BOOST_CHECK_EQUAL(resp.body(), "raw string body");
        BOOST_CHECK_EQUAL(
            std::string(resp[boost::beast::http::field::content_type]),
            "text/plain");
    }

    // Handler error becomes a 500 with the message.
    {
        auto resp = bridge.handle(make_request("GET", "/boom"));
        BOOST_CHECK_EQUAL(resp.result_int(), 500);
        BOOST_CHECK(resp.body().find("handler exploded") != std::string::npos);
    }

    // Unknown route handled by the bridge itself.
    {
        auto resp = bridge.handle(make_request("GET", "/missing"));
        BOOST_CHECK_EQUAL(resp.result_int(), 404);
    }

    bridge.detach();
    manager.shutdown_all("done");
}

// ---------------------------------------------------------------------------
// Route cleanup on service exit.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(RoutesRemovedWhenServiceExits) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const std::string module = write_script("cov_httpd_short.lua", R"lua(
local M = {}
function M.on_init()
    shield.httpd.get("/only", function(req) return { status = 200 } end)
end
return M
)lua");
    auto svc = manager.spawn(
        module, R"({"name":"cov_httpd_short","args":{},"config":{}})");
    BOOST_REQUIRE(svc.success);
    BOOST_REQUIRE_EQUAL(runtime.http_route_count(), 1u);

    manager.exit(svc.service_id, "normal");
    // Exit is asynchronous; wait for the route table to drain.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (runtime.http_route_count() > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    BOOST_CHECK_EQUAL(runtime.http_route_count(), 0u);
}

// Routes registered before the bridge has a server attached take the
// register_on_server early-return; attach() replays them afterwards so the
// route still serves requests.
BOOST_AUTO_TEST_CASE(PreAttachRegistrationReplayedByAttach) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const char* script = R"lua(
local M = {}
function M.on_init()
    shield.httpd.put("/preattach", function(req)
        return { status = 200, body = "preattach-ok" }
    end)
end
return M
)lua";
    const std::string module = write_script("cov_httpd_pre.lua", script);
    auto svc = manager.spawn(
        module, R"({"name":"cov_httpd_pre","args":{},"config":{}})");
    BOOST_REQUIRE_MESSAGE(svc.success, "spawn failed: " + svc.error_message);
    BOOST_REQUIRE_EQUAL(runtime.http_route_count(), 1u);

    shield::net::HttpServer server;
    LuaHttpBridge bridge(runtime, manager);
    bridge.attach(server);  // replays the stored route onto the server

    auto resp = bridge.handle(make_request("PUT", "/preattach"));
    BOOST_CHECK_EQUAL(resp.result_int(), 200);
    BOOST_CHECK_EQUAL(resp.body(), "preattach-ok");

    bridge.detach();
    manager.shutdown_all("done");
}
