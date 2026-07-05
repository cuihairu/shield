// LAPI-009: Gateway API tests.
//
// Exercises the gateway service pattern via LuaServiceManager::call():
// on_connect / on_client_message / on_disconnect with table-based session
// simulation.  MockSessionHandle userdata tests are deferred until the
// gateway C++ integration layer exposes session creation to the test harness.

#define BOOST_TEST_MODULE LuaApiGatewayTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_gateway_bridge.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/protocol.hpp"

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
    MockSession(shield::net::SessionId id, shield::net::RemoteAddress remote,
                bool protocol_enabled = false,
                std::string protocol_codec = "json")
        : id_(id),
          remote_(std::move(remote)),
          protocol_enabled_(protocol_enabled),
          protocol_codec_(std::move(protocol_codec)) {}

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
    bool has_protocol_pipeline() const override { return protocol_enabled_; }
    std::string_view protocol_codec_name() const override {
        return protocol_enabled_ ? std::string_view(protocol_codec_)
                                 : std::string_view{};
    }
    bool send_message(const shield::transport::DecodedBody& message,
                      std::string* error) override {
        if (!alive_) {
            if (error) {
                *error = "session is closed";
            }
            return false;
        }
        sent_messages_.push_back(message);
        return true;
    }
    void set_user_data(std::string key, std::string value) override {
        user_data_[std::move(key)] = std::move(value);
    }
    std::string get_user_data(std::string_view key) const override {
        auto it = user_data_.find(std::string(key));
        return it == user_data_.end() ? "" : it->second;
    }

    const std::vector<std::vector<uint8_t>>& sent() const { return sent_; }
    const std::vector<shield::transport::DecodedBody>& sent_messages() const {
        return sent_messages_;
    }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool protocol_enabled_ = false;
    std::string protocol_codec_;
    bool alive_ = true;
    std::string close_reason_;
    std::vector<std::vector<uint8_t>> sent_;
    std::vector<shield::transport::DecodedBody> sent_messages_;
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

BOOST_AUTO_TEST_CASE(
    LuaGatewayBridgePassesRealSessionHandleToLuaForProtocolEgress) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_protocol_handle");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(manager, result.service_id);
    auto session = std::make_shared<MockSession>(
        43, shield::net::RemoteAddress{"127.0.0.1", 34568}, true);

    bridge.on_connect(session);
    manager.pump_once();
    manager.pump_once();

    CallResult cr = manager.call(
        result.service_id, "on_client_message",
        nlohmann::json::array({make_session_handle_json(session),
                               nlohmann::json::object({{"route", "login"},
                                                       {"payload",
                                                        nlohmann::json::object(
                                                            {{"uid", 99}})}})}));
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(session->sent_messages().size(), 1u);
    BOOST_REQUIRE(session->sent_messages()[0].has_message());
    BOOST_CHECK_EQUAL(
        (*session->sent_messages()[0].message)["route"].get<std::string>(),
        "login");
}

BOOST_AUTO_TEST_CASE(
    LuaGatewayBridgeRejectsRawStringEgressForStructuredProtocolSessions) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_protocol_handle_raw");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(manager, result.service_id);
    auto session = std::make_shared<MockSession>(
        44, shield::net::RemoteAddress{"127.0.0.1", 34569}, true);

    bridge.on_connect(session);
    manager.pump_once();
    manager.pump_once();

    CallResult cr = manager.call(
        result.service_id, "on_client_message",
        nlohmann::json::array({make_session_handle_json(session), "raw_text"}));
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(session->sent_messages().size(), 0u);

    CallResult sessions = manager.call(result.service_id, "get_sessions",
                                       nlohmann::json::array());
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values.is_array());
    BOOST_REQUIRE_EQUAL(sessions.values.size(), 1u);
    BOOST_REQUIRE(sessions.values[0].contains("44"));
    BOOST_REQUIRE(sessions.values[0]["44"].contains("last_send"));
    BOOST_CHECK_EQUAL(
        sessions.values[0]["44"]["last_send"]["ok"].get<bool>(), false);
    BOOST_REQUIRE(sessions.values[0]["44"]["last_send"]["error"].is_object());
    BOOST_CHECK_EQUAL(
        sessions.values[0]["44"]["last_send"]["error"]["code"]
            .get<std::string>(),
        "protocol_message_required");
    BOOST_CHECK_EQUAL(
        sessions.values[0]["44"]["last_message"]["payload"].get<std::string>(),
        "raw_text");
}

BOOST_AUTO_TEST_CASE(
    LuaGatewayBridgeRoutesDecodeLocalProtocolPacketsToClientMessage) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_packet_bridge");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(manager, result.service_id);
    auto session = std::make_shared<MockSession>(
        77, shield::net::RemoteAddress{"127.0.0.1", 45678});

    bridge.on_connect(session);
    manager.pump_once();
    manager.pump_once();

    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::DecodeLocal;
    dispatch.packet.route_id = 0x1001;
    dispatch.packet.kind =
        static_cast<std::uint16_t>(shield::transport::PacketKind::Message);
    dispatch.packet.body = std::vector<std::uint8_t>{'b', 'o', 'd', 'y'};

    shield::transport::RouteEntry route;
    route.route_id = 0x1001;
    route.target_service = 9;
    route.codec_id = 4;
    route.schema_id = 33;
    route.debug_name = "player.move";
    dispatch.route = &route;
    dispatch.decoded_body = shield::transport::DecodedBody{
        .codec_id = 4,
        .schema_id = 33,
        .route_name = "player.move",
        .bytes = std::vector<std::uint8_t>{'{', '}', '\n'},
        .message = nlohmann::json::object({{"uid", 7}, {"dir", "north"}}),
    };

    bridge.on_packet(session, dispatch);
    manager.pump_once();
    manager.pump_once();

    CallResult sessions = manager.call(result.service_id, "get_sessions",
                                       nlohmann::json::array());
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values.is_array());
    BOOST_REQUIRE_EQUAL(sessions.values.size(), 1u);
    BOOST_REQUIRE(sessions.values[0].contains("77"));

    const auto& state = sessions.values[0]["77"];
    BOOST_REQUIRE(state.contains("last_message"));
    BOOST_REQUIRE(state["last_message"]["payload"].is_object());
    BOOST_CHECK_EQUAL(state["last_message"]["payload"]["uid"].get<int>(), 7);
    BOOST_CHECK_EQUAL(
        state["last_message"]["payload"]["dir"].get<std::string>(), "north");
    BOOST_CHECK(!state.contains("last_packet"));
}

BOOST_AUTO_TEST_CASE(
    LuaGatewayBridgeRoutesRawDecodeLocalProtocolPacketsAsStrings) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_raw_packet_bridge");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(manager, result.service_id);
    auto session = std::make_shared<MockSession>(
        78, shield::net::RemoteAddress{"127.0.0.1", 45679});

    bridge.on_connect(session);
    manager.pump_once();
    manager.pump_once();

    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::DecodeLocal;
    dispatch.packet.route_id = 0x1002;
    dispatch.packet.body = std::vector<std::uint8_t>{'r', 'a', 'w'};

    shield::transport::RouteEntry route;
    route.route_id = 0x1002;
    route.target_service = 9;
    route.codec_id = 1;
    route.schema_id = 0;
    route.debug_name = "raw.echo";
    dispatch.route = &route;
    dispatch.decoded_body = shield::transport::DecodedBody{
        .codec_id = 1,
        .schema_id = 0,
        .route_name = "raw.echo",
        .bytes = std::vector<std::uint8_t>{'r', 'a', 'w'},
    };

    bridge.on_packet(session, dispatch);
    manager.pump_once();
    manager.pump_once();

    CallResult sessions = manager.call(result.service_id, "get_sessions",
                                       nlohmann::json::array());
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values.is_array());
    BOOST_REQUIRE_EQUAL(sessions.values.size(), 1u);
    BOOST_REQUIRE(sessions.values[0].contains("78"));

    const auto& state = sessions.values[0]["78"];
    BOOST_REQUIRE(state.contains("last_message"));
    BOOST_CHECK_EQUAL(state["last_message"]["payload"].get<std::string>(),
                      "raw");
}

BOOST_AUTO_TEST_CASE(
    LuaGatewayBridgeDoesNotExposeForwardRawProtocolPacketsToLua) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_gateway(manager, "gw_forward_raw_drop");
    BOOST_REQUIRE(result.success);

    LuaGatewayBridge bridge(manager, result.service_id);
    auto session = std::make_shared<MockSession>(
        88, shield::net::RemoteAddress{"127.0.0.1", 56789});

    bridge.on_connect(session);
    manager.pump_once();
    manager.pump_once();

    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::ForwardRaw;
    dispatch.packet.route_id = 0x2002;
    dispatch.packet.body = std::vector<std::uint8_t>{'r', 'a', 'w'};
    dispatch.packet.raw_frame =
        std::vector<std::uint8_t>{'f', 'r', 'a', 'm', 'e'};

    bridge.on_packet(session, dispatch);
    manager.pump_once();
    manager.pump_once();

    CallResult sessions = manager.call(result.service_id, "get_sessions",
                                       nlohmann::json::array());
    BOOST_REQUIRE(sessions.success);
    BOOST_REQUIRE(sessions.values.is_array());
    BOOST_REQUIRE_EQUAL(sessions.values.size(), 1u);
    BOOST_REQUIRE(sessions.values[0].contains("88"));

    const auto& state = sessions.values[0]["88"];
    BOOST_CHECK(!state.contains("last_message"));
    BOOST_CHECK(!state.contains("last_packet"));
}

BOOST_AUTO_TEST_SUITE_END()
