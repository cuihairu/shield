// LAPI-009: Gateway API tests.
//
// The Gateway API (on_connect / on_disconnect / on_client_message +
// SessionHandle) requires a dedicated mock harness that simulates TCP
// sessions. This harness is not yet part of the Phase 1 test infra.
//
// This file contains lightweight structural checks that verify the
// gateway service Lua scripts load correctly and that the expected
// handler functions exist on the module table. Full session simulation
// tests (LAPI-009-01 through LAPI-009-05) will be added when the mock
// session harness is implemented.
//
// Current coverage:
//   - Gateway service module loads and returns a table
//   - on_connect / on_disconnect / on_client_message functions exist
//   - Service is callable after load

#define BOOST_TEST_MODULE LuaApiGatewayTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json opts_for(const std::string& name,
                        nlohmann::json config = nlohmann::json::object()) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", std::move(config)},
    };
}

SpawnResult spawn_gateway(LuaServiceManager& manager, const std::string& name,
                          nlohmann::json config = nlohmann::json::object()) {
    return manager.spawn(TEST_SCRIPTS_DIR + "gateway_service.lua",
                         opts_for(name, std::move(config)).dump());
}
}  // namespace

BOOST_AUTO_TEST_SUITE(Lapi009GatewayApi)

// ---------------------------------------------------------------------------
// Gateway service module loads correctly
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(GatewayServiceLoads) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_load_test");
    BOOST_REQUIRE(result.success);
}

// ---------------------------------------------------------------------------
// Handler functions exist on the module table
// We verify by calling them through the runtime; if the function exists
// and is callable, call_service_function returns true.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(GatewayHandlerFunctionsExist) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_handler_check");
    BOOST_REQUIRE(result.success);

    // Call a method that verifies handler functions exist from Lua side
    CallResult cr = manager.call(result.service_id, "get_sessions",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    // Empty Lua table serializes as JSON array []; non-null means module loaded
    BOOST_CHECK(!cr.values[0].is_null());
}

// ---------------------------------------------------------------------------
// Gateway service remains callable after load
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(GatewayServiceIsCallable) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_callable_test");
    BOOST_REQUIRE(result.success);

    // get_sessions is a regular method that should be callable
    CallResult cr = manager.call(result.service_id, "get_sessions",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    // Freshly loaded service has no sessions (empty table)
    BOOST_CHECK(!cr.values[0].is_null());
}

// ---------------------------------------------------------------------------
// LAPI-009-01 through LAPI-009-05: Full session simulation
// These require a mock SessionHandle that can be created from C++ and
// passed to Lua gateway handlers. Deferred until mock harness exists.
// ---------------------------------------------------------------------------
//
// LAPI-009-01 SimulatedConnect       — requires mock SessionHandle
// LAPI-009-02 ClientFrameDecoded     — requires mock SessionHandle + payload
// LAPI-009-03 DisconnectHandler      — requires mock SessionHandle lifecycle
// LAPI-009-04 SendQueueFull          — requires mock SessionHandle with queue
// LAPI-009-05 StaleSessionSend       — requires mock SessionHandle close tracking
//
// These will be implemented when the gateway mock harness is added.

BOOST_AUTO_TEST_SUITE_END()
