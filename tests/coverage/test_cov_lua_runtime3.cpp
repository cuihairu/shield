// Coverage tests (round 3) for src/lua/lua_runtime.cpp: the M4
// invoke_coroutine synchronous-degrade paths. A VM without the shield API
// (no __shield_run_handler coroutine factory) cannot host yieldable
// handlers, so dispatch falls back to a plain synchronous call that routes
// completion through the same finish_ok / finish_err helpers.
#define BOOST_TEST_MODULE CovLuaRuntime3
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

// A bare VM: runtime.create_vm() opens the base libraries but does not
// register the shield API, so __shield_run_handler is absent.
struct BareVm {
    caf::actor_system_config cfg;
    caf::actor_system system;
    LuaRuntime runtime;
    std::shared_ptr<LuaVM> vm;

    BareVm() : system(cfg), vm(runtime.create_vm()) {}
};

}  // namespace

// Successful synchronous dispatch on a VM without a coroutine factory: the
// with-ctx variant builds the dispatch context table, and completion routes
// through finish_ok (complete_call + reset_error_count — no-ops for an
// unknown session/service).
BOOST_AUTO_TEST_CASE(BareVmSyncSuccessRoutesThroughFinishOk) {
    BareVm fx;
    sol::state_view lua(fx.runtime.vm_state(fx.vm).lua_state());
    lua.script("function add(ctx, a, b) return a + b, 'tail' end");
    sol::function fn = lua["add"];

    std::string error;
    const bool ok = fx.runtime.invoke_coroutine(fx.vm, fn, {2, 3}, "handler",
                                                "add", 0, nullptr, "", &error,
                                                /*prepend_ctx=*/true);
    if (!ok) std::fprintf(stderr, "bare sync error: %s\n", error.c_str());
    BOOST_CHECK(ok);
}

// A synchronous handler error routes through finish_err: the message lands
// in *error, and with a manager + service id attached it also invokes the
// (absent) error hook and consumes any (absent) pending exit — all safe
// no-ops for an unknown service.
BOOST_AUTO_TEST_CASE(BareVmSyncErrorRoutesThroughFinishErr) {
    BareVm fx;
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state_view lua(fx.runtime.vm_state(fx.vm).lua_state());
    lua.script("function boom() error('kaput') end");
    sol::function fn = lua["boom"];

    std::string error;
    BOOST_CHECK(!fx.runtime.invoke_coroutine(fx.vm, fn, {}, "timer", "boom", 0,
                                             nullptr, "", &error,
                                             /*prepend_ctx=*/false));
    BOOST_CHECK(error.find("kaput") != std::string::npos);

    // Same failure with completion routing attached: complete_call and
    // invoke_error_hook on an unknown session/service are no-ops.
    std::string error2;
    BOOST_CHECK(!fx.runtime.invoke_coroutine(fx.vm, fn, {}, "timer", "boom",
                                             987654, &manager, "ghost_service",
                                             &error2, false));
    BOOST_CHECK(error2.find("kaput") != std::string::npos);
}

// A failing __shield_run_handler factory: degrade_on_factory_failure=false
// surfaces the factory error through finish_err; true re-dispatches the
// handler synchronously without ctx.
BOOST_AUTO_TEST_CASE(FactoryFailureDegradePaths) {
    BareVm fx;
    sol::state_view lua(fx.runtime.vm_state(fx.vm).lua_state());
    lua.script(R"lua(
        function plain(x) return (x or 0) + 1 end
        __shield_run_handler = function() error('factory kaput') end
    )lua");
    sol::function fn = lua["plain"];

    std::string error;
    BOOST_CHECK(!fx.runtime.invoke_coroutine(
        fx.vm, fn, {1}, "handler", "plain", 0, nullptr, "", &error,
        /*prepend_ctx=*/true,
        /*degrade_on_factory_failure=*/false));
    BOOST_CHECK(error.find("factory kaput") != std::string::npos);

    std::string error2;
    const bool ok2 = fx.runtime.invoke_coroutine(
        fx.vm, fn, {1}, "handler", "plain", 0, nullptr, "", &error2, true,
        /*degrade_on_factory_failure=*/true);
    if (!ok2) std::fprintf(stderr, "degrade error: %s\n", error2.c_str());
    BOOST_CHECK(ok2);
}

// A factory that returns a non-thread value cannot be resumed: without the
// degrade path the dispatch fails with a dedicated message; with it, the
// handler still runs synchronously.
BOOST_AUTO_TEST_CASE(FactoryReturnsNonThreadPaths) {
    BareVm fx;
    sol::state_view lua(fx.runtime.vm_state(fx.vm).lua_state());
    lua.script(R"lua(
        function plain() return 'ok' end
        __shield_run_handler = function(handler, args) return {} end
    )lua");
    sol::function fn = lua["plain"];

    std::string error;
    BOOST_CHECK(!fx.runtime.invoke_coroutine(fx.vm, fn, {}, "handler", "plain",
                                             0, nullptr, "", &error, true,
                                             false));
    BOOST_CHECK(error == "handler coroutine thread missing");

    BOOST_CHECK(fx.runtime.invoke_coroutine(fx.vm, fn, {}, "handler", "plain",
                                            0, nullptr, "", nullptr, true,
                                            true));
}

// On a factory-bearing VM a nil handler fails fast with a dedicated message.
BOOST_AUTO_TEST_CASE(HandlerNotFunctionOnFactoryVm) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto vm = runtime.create_vm();
    register_full_shield_api(runtime.vm_state(vm), &manager, &runtime);

    sol::state_view lua(runtime.vm_state(vm).lua_state());
    sol::object nil_fn = sol::make_object(lua, sol::nil);
    std::string error;
    BOOST_CHECK(!runtime.invoke_coroutine(vm, nil_fn, {}, "handler", "m", 0,
                                          nullptr, "", &error, true));
    BOOST_CHECK(error == "handler is not a function");
}

// resolve_service_method on a VM that never loaded a service module reports
// "service module not loaded" (the service table is sol::nil).
BOOST_AUTO_TEST_CASE(CallServiceMethodWithoutModuleFails) {
    BareVm fx;
    nlohmann::json returns;
    std::string error;
    BOOST_CHECK(!fx.runtime.call_service_method(fx.vm, "any_method", {},
                                                &returns, &error));
    BOOST_CHECK(error == "service module not loaded");
}

// An HTTP route whose handler sol::function went invalid reports "handler is
// not callable"; a Lua-level handler error surfaces as out_desc["lua_error"].
BOOST_AUTO_TEST_CASE(CallHttpHandlerInvalidAndLuaError) {
    BareVm fx;
    sol::state_view lua(fx.runtime.vm_state(fx.vm).lua_state());
    lua.script("function bad_handler(ctx, req) error('http kaput') end");

    std::string reg_err;
    BOOST_CHECK(fx.runtime.register_http_route(
        fx.vm, "cov3.http", "GET", "/cov3-bad", lua["bad_handler"], &reg_err));

    auto route = fx.runtime.find_http_route("GET", "/cov3-bad", nullptr);
    BOOST_REQUIRE(route.has_value());

    // Corrupt the stored handler, then observe the guard.
    route->handler = nullptr;
    nlohmann::json desc;
    std::string error;
    BOOST_CHECK(
        !fx.runtime.call_http_handler(*route, {{"path", "/x"}}, desc, &error));
    BOOST_CHECK(error == "handler is not callable");

    // A fresh lookup runs the real handler, which throws: the error becomes
    // out_desc["lua_error"] and the call still reports success (the HTTP
    // layer maps it to a 500 itself).
    auto route2 = fx.runtime.find_http_route("GET", "/cov3-bad", nullptr);
    BOOST_REQUIRE(route2.has_value());
    nlohmann::json desc2;
    std::string error2;
    BOOST_CHECK(fx.runtime.call_http_handler(*route2, {{"path", "/x"}}, desc2,
                                             &error2));
    BOOST_CHECK(desc2.contains("lua_error"));
    BOOST_CHECK(desc2["lua_error"].get<std::string>().find("http kaput") !=
                std::string::npos);
    fx.runtime.remove_http_routes_for_service("cov3.http");
}
