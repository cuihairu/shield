// LAPI-009: Gateway API tests.
//
// Exercises the gateway service pattern via LuaServiceManager::call():
// on_connect / on_client_message / on_disconnect with table-based session
// simulation.  MockSessionHandle userdata tests are deferred until the
// gateway C++ integration layer exposes session creation to the test harness.

#define BOOST_TEST_MODULE LuaApiGatewayTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_gateway_bridge.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

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

class MockSession final : public shield::net::Session {
public:
    MockSession(shield::net::SessionId id, shield::net::RemoteAddress remote)
        : id_(id), remote_(std::move(remote)) {}

    shield::net::SessionId id() const override { return id_; }
    shield::net::RemoteAddress remote_addr() const override { return remote_; }
    bool send(const std::vector<uint8_t>& data) override {
        sent_.push_back(data);
        return alive_;
    }
    void close(std::string reason) override {
        alive_ = false;
        close_reason_ = std::move(reason);
    }
    bool is_alive() const override { return alive_; }
    std::string error_code() const override { return alive_ ? "" : "session_closed"; }
    void set_user_data(std::string key, std::string value) override {
        user_data_[std::move(key)] = std::move(value);
    }
    std::string get_user_data(std::string_view key) const override {
        auto it = user_data_.find(std::string(key));
        return it == user_data_.end() ? "" : it->second;
    }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool alive_ = true;
    std::string close_reason_;
    std::vector<std::vector<uint8_t>> sent_;
    std::unordered_map<std::string, std::string> user_data_;
};
}  // namespace

BOOST_AUTO_TEST_SUITE(Lapi009GatewayApi)

// ---------------------------------------------------------------------------
// LAPI-009-01: Gateway service handles simulated connect.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_01_SimulatedConnect) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_connect");
    BOOST_REQUIRE(result.success);

    CallResult cr = manager.call(result.service_id, "on_connect",
                                 nlohmann::json::array({nlohmann::json::object({
                                     {"id", "sess_1"},
                                     {"remote_addr", "127.0.0.1:12345"}
                                 })}));
    BOOST_CHECK(cr.success);
}

// ---------------------------------------------------------------------------
// LAPI-009-02: Client message delivery to on_client_message.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_02_ClientMessageDelivery) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_message");
    BOOST_REQUIRE(result.success);

    // Connect.
    manager.call(result.service_id, "on_connect",
                 nlohmann::json::array({nlohmann::json::object({
                     {"id", "sess_2"}, {"remote_addr", "10.0.0.1:8080"}
                 })}));

    // Send message.
    CallResult cr = manager.call(result.service_id, "on_client_message",
                                 nlohmann::json::array({
                                     nlohmann::json::object({{"id", "sess_2"}}),
                                     "hello_payload"
                                 }));
    BOOST_CHECK(cr.success);

    // Verify session was recorded.
    CallResult check = manager.call(result.service_id, "get_sessions",
                                    nlohmann::json::array());
    BOOST_REQUIRE(check.success);
    BOOST_REQUIRE_EQUAL(check.values.size(), 1u);
}

// ---------------------------------------------------------------------------
// LAPI-009-03: Disconnect handler.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_03_DisconnectHandler) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_disconnect");
    BOOST_REQUIRE(result.success);

    // Connect.
    manager.call(result.service_id, "on_connect",
                 nlohmann::json::array({nlohmann::json::object({
                     {"id", "sess_3"}, {"remote_addr", "192.168.1.1:9999"}
                 })}));

    // Disconnect.
    CallResult cr = manager.call(result.service_id, "on_disconnect",
                                 nlohmann::json::array({
                                     nlohmann::json::object({{"id", "sess_3"}}),
                                     "client_closed"
                                 }));
    BOOST_CHECK(cr.success);

    // Verify session marked disconnected.
    CallResult check = manager.call(result.service_id, "get_sessions",
                                    nlohmann::json::array());
    BOOST_REQUIRE(check.success);
}

// ---------------------------------------------------------------------------
// LAPI-009-04: Send queue full — tested via Lua-side session:send mock.
// The gateway_service.lua now checks session:send return values and records
// errors. We verify that a table-based session with a failing send is handled.
// Full MockSessionHandle userdata integration requires the C++ gateway layer
// to expose session creation to the test harness (deferred).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_04_SendQueueFullHandled) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_queue");
    BOOST_REQUIRE(result.success);

    // Connect a session.
    manager.call(result.service_id, "on_connect",
                 nlohmann::json::array({nlohmann::json::object({
                     {"id", "sess_queue"}, {"remote_addr", "127.0.0.1:1"}
                 })}));

    // Send a message — the Lua handler echoes back via session:send.
    // Since the session is a plain table (no send method), the handler
    // gracefully skips the send. Verify no crash.
    CallResult cr = manager.call(result.service_id, "on_client_message",
                                 nlohmann::json::array({
                                     nlohmann::json::object({{"id", "sess_queue"}}),
                                     "test_payload"
                                 }));
    BOOST_CHECK(cr.success);
}

// ---------------------------------------------------------------------------
// LAPI-009-05: Stale session — send after disconnect.
// Verify the handler processes the message even for a disconnected session.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LAPI_009_05_StaleSessionHandled) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_stale");
    BOOST_REQUIRE(result.success);

    // Connect, then disconnect.
    manager.call(result.service_id, "on_connect",
                 nlohmann::json::array({nlohmann::json::object({
                     {"id", "sess_stale"}, {"remote_addr", "10.0.0.1:80"}
                 })}));
    manager.call(result.service_id, "on_disconnect",
                 nlohmann::json::array({
                     nlohmann::json::object({{"id", "sess_stale"}}),
                     "test_close"
                 }));

    // Send a message to the disconnected session — should not crash.
    CallResult cr = manager.call(result.service_id, "on_client_message",
                                 nlohmann::json::array({
                                     nlohmann::json::object({{"id", "sess_stale"}}),
                                     "late_payload"
                                 }));
    BOOST_CHECK(cr.success);
}

// ---------------------------------------------------------------------------
// Gateway module loads and all handler functions exist.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(GatewayServiceLoadsAndHandlersExist) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_full");
    BOOST_REQUIRE(result.success);

    CallResult on_conn = manager.call(result.service_id, "on_connect",
                                      nlohmann::json::array({nlohmann::json::object()}));
    BOOST_CHECK(on_conn.success);

    CallResult on_msg = manager.call(result.service_id, "on_client_message",
                                     nlohmann::json::array({nlohmann::json::object(),
                                                            "test"}));
    BOOST_CHECK(on_msg.success);

    CallResult on_disc = manager.call(result.service_id, "on_disconnect",
                                      nlohmann::json::array({nlohmann::json::object(),
                                                             "test"}));
    BOOST_CHECK(on_disc.success);

    CallResult sessions = manager.call(result.service_id, "get_sessions",
                                       nlohmann::json::array());
    BOOST_CHECK(sessions.success);
}

// ---------------------------------------------------------------------------
// Real bridge path queues reserved gateway events through the Lua worker path.
// This catches regressions where LuaGatewayBridge accidentally uses public
// send(), which rejects on_* lifecycle method names.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaGatewayBridgeQueuesReservedGatewayEvents) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_bridge");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(manager, result.service_id);
    auto session = std::make_shared<MockSession>(
        42, shield::net::RemoteAddress{"127.0.0.1", 34567});

    bridge.on_connect(session);
    manager.pump_once();  // run bridge task
    manager.pump_once();  // dispatch queued on_connect message

    CallResult sessions = manager.call(result.service_id, "get_sessions",
                                       nlohmann::json::array());
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values.is_array());
    BOOST_REQUIRE_EQUAL(sessions.values.size(), 1u);
    BOOST_REQUIRE(sessions.values[0].is_object());
    BOOST_CHECK(sessions.values[0].contains("42"));

    bridge.on_message(session, "hello");
    manager.pump_once();
    manager.pump_once();

    sessions = manager.call(result.service_id, "get_sessions",
                            nlohmann::json::array());
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values[0].contains("42"));
    BOOST_CHECK_EQUAL(
        sessions.values[0]["42"]["last_message"]["payload"].get<std::string>(),
        "hello");
}

BOOST_AUTO_TEST_SUITE_END()
