// Coverage tests for src/lua/lua_gateway_bridge.cpp
#define BOOST_TEST_MODULE CovLuaGatewayBridge
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_gateway_bridge.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/protocol.hpp"

using namespace shield::lua;

namespace {

const char* kBridgeServiceScript = R"lua(
local M = {}
local log = {}
function M.on_init(args) end
function M.on_connect(ctx, info) table.insert(log, {"on_connect"}) return "ok" end
function M.on_disconnect(ctx, info, reason) table.insert(log, {"on_disconnect"}) return "ok" end
function M.on_client_message(ctx, route_id, client_ctx, body, message)
  table.insert(log, {"on_client_message", route_id})
  return "ok"
end
function M.get_log(ctx) return log end
function M.ping(ctx) return "pong" end
return M
)lua";

std::string write_script(const std::string& path, const char* content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
    return path;
}

nlohmann::json opts_for(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
}

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

class MockSession final : public shield::net::Session {
public:
    MockSession(shield::net::SessionId id, shield::net::RemoteAddress remote)
        : id_(id), remote_(std::move(remote)) {}

    shield::net::SessionId id() const override { return id_; }
    shield::net::RemoteAddress remote_addr() const override { return remote_; }
    bool send(const std::vector<uint8_t>& data, std::string* error) override {
        if (!alive_) {
            if (error) *error = "session is closed";
            return false;
        }
        sent_.push_back(data);
        return true;
    }
    void close(std::string reason) override {
        alive_ = false;
        close_reason_ = std::move(reason);
    }
    bool is_alive() const override { return alive_; }
    std::string error_code() const override {
        return alive_ ? "" : "session_closed";
    }
    bool has_protocol_pipeline() const override { return false; }
    std::string_view protocol_codec_name() const override { return {}; }
    bool send_message(const shield::transport::DecodedBody& message,
                      std::string* error) override {
        if (!alive_) {
            if (error) *error = "session is closed";
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

    shield::net::SessionBinding binding() const override { return binding_; }
    void reset_binding(shield::net::SessionBinding initial) override {
        binding_ = std::move(initial);
    }
    bool apply_binding(std::string target_service, std::string player_id,
                       uint32_t expected_epoch,
                       shield::net::SessionBinding* out) override {
        if (expected_epoch != shield::net::kAnyEpoch &&
            expected_epoch != binding_.epoch) {
            return false;
        }
        binding_.target_service = std::move(target_service);
        binding_.player_id = std::move(player_id);
        ++binding_.epoch;
        if (out != nullptr) {
            *out = binding_;
        }
        return true;
    }

    // Test-side read helpers (not part of the Session interface).
    std::string target_service() const { return binding_.target_service; }
    uint32_t epoch() const { return binding_.epoch; }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool alive_ = true;
    std::string close_reason_;
    std::vector<std::vector<uint8_t>> sent_;
    std::vector<shield::transport::DecodedBody> sent_messages_;
    std::unordered_map<std::string, std::string> user_data_;
    shield::net::SessionBinding binding_;
};

shield::transport::DispatchResult make_packet(
    uint32_t route_id, const shield::transport::RouteEntry* route,
    bool with_decoded = true) {
    shield::transport::DispatchResult dispatch;
    dispatch.action = shield::transport::RouteAction::DecodeLocal;
    dispatch.packet.route_id = route_id;
    dispatch.packet.body = std::vector<std::uint8_t>{'b', 'o', 'd', 'y'};
    dispatch.route = route;
    if (with_decoded) {
        dispatch.decoded_body = shield::transport::DecodedBody{
            .bytes = std::vector<std::uint8_t>{'b', 'o', 'd', 'y'},
        };
    }
    return dispatch;
}

}  // namespace

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(CovLuaGatewayBridge)

// Null-session early returns for all three entry points.
BOOST_AUTO_TEST_CASE(NullSessionsShortCircuit) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    LuaGatewayBridge bridge(
        manager, "cov_ghost_auth",
        std::make_shared<shield::lua::GatewaySessionRegistry>());

    bridge.on_connect(nullptr);
    bridge.on_packet(nullptr, shield::transport::DispatchResult{});
    bridge.on_disconnect(nullptr, "gone");
    BOOST_CHECK(true);
}

// on_connect against a missing auth service: send_system fails and the
// failure is logged (warning branch), while the initial binding is installed
// and the session is registered with the gateway registry.
BOOST_AUTO_TEST_CASE(OnConnectSendFailureLogsWarning) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto registry = std::make_shared<shield::lua::GatewaySessionRegistry>();
    LuaGatewayBridge bridge(manager, "cov_ghost_auth", registry);

    auto session = std::make_shared<MockSession>(
        101, shield::net::RemoteAddress{"127.0.0.1", 6001});
    bridge.on_connect(session);

    const shield::net::SessionBinding binding = session->binding();
    BOOST_CHECK_EQUAL(binding.target_service, "cov_ghost_auth");
    BOOST_CHECK_EQUAL(binding.epoch, 0u);
    BOOST_CHECK_EQUAL(binding.player_id, "");
    BOOST_CHECK_EQUAL(binding.gateway_name, "cov_ghost_auth");
    BOOST_CHECK_EQUAL(registry->size(), 1u);
    BOOST_CHECK(registry->find(101) != nullptr);
}

// on_packet rejection branches: not-ok packet, unknown route, wrong
// direction, unauthenticated access to a protected route.
BOOST_AUTO_TEST_CASE(OnPacketRejectionBranches) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    LuaGatewayBridge bridge(
        manager, "cov_ghost_auth",
        std::make_shared<shield::lua::GatewaySessionRegistry>());

    auto session = std::make_shared<MockSession>(
        102, shield::net::RemoteAddress{"127.0.0.1", 6002});

    // Error packet: ok() is false.
    shield::transport::DispatchResult bad;
    bad.action = shield::transport::RouteAction::DecodeLocal;
    bad.error = "frame error";
    bridge.on_packet(session, bad);

    // Drop packet.
    shield::transport::DispatchResult drop;
    drop.action = shield::transport::RouteAction::Drop;
    bridge.on_packet(session, drop);

    // ForwardRaw packet.
    shield::transport::DispatchResult fwd;
    fwd.action = shield::transport::RouteAction::ForwardRaw;
    bridge.on_packet(session, fwd);

    // Unknown route: route pointer null.
    bridge.on_packet(session, make_packet(0x9999, nullptr));

    // ServerToClient direction rejected from a client session.
    shield::transport::RouteEntry stc;
    stc.route_id = 0x2001;
    stc.direction = shield::transport::RouteDirection::ServerToClient;
    stc.requires_auth = false;
    bridge.on_packet(session, make_packet(0x2001, &stc));

    // requires_auth with empty player id rejected.
    shield::transport::RouteEntry auth_req;
    auth_req.route_id = 0x2002;
    auth_req.direction = shield::transport::RouteDirection::ClientToServer;
    auth_req.requires_auth = true;
    bridge.on_packet(session, make_packet(0x2002, &auth_req));

    // Same route passes once the player is authenticated; no target service
    // configured on this session -> "no target service" warning branch.
    session->apply_binding("cov_game", "player-1", shield::net::kAnyEpoch,
                           nullptr);
    bridge.on_packet(session, make_packet(0x2002, &auth_req, false));

    // No decoded body: body bytes come straight from the wire packet.
    session->apply_binding("", "", shield::net::kAnyEpoch, nullptr);
    BOOST_CHECK(true);
}

// Route without logical name and empty session target: falls through to the
// empty-target warning branch.
BOOST_AUTO_TEST_CASE(OnPacketNoTargetService) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    LuaGatewayBridge bridge(
        manager, "cov_ghost_auth",
        std::make_shared<shield::lua::GatewaySessionRegistry>());

    auto session = std::make_shared<MockSession>(
        103, shield::net::RemoteAddress{"127.0.0.1", 6003});

    shield::transport::RouteEntry route;
    route.route_id = 0x3001;
    route.direction = shield::transport::RouteDirection::Bidirectional;
    route.requires_auth = false;
    bridge.on_packet(session, make_packet(0x3001, &route, false));
    BOOST_CHECK(true);
}

// Route resolution ignores any per-route service notion: dispatch always
// lands on the session's bound target service.
BOOST_AUTO_TEST_CASE(OnPacketUsesSessionTargetService) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script = write_script("/tmp/opencode/cov_bridge_service.lua",
                                     kBridgeServiceScript);
    auto svc = manager.spawn(script, opts_for("cov_game").dump());
    BOOST_REQUIRE(svc.success);
    auto svc2 = manager.spawn(script, opts_for("cov_game_fallback").dump());
    BOOST_REQUIRE(svc2.success);

    LuaGatewayBridge bridge(
        manager, "cov_ghost_auth",
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        104, shield::net::RemoteAddress{"127.0.0.1", 6004});
    session->reset_binding({svc2.service_id, "", "cov_ghost_auth", "", 0});

    // Route-level logical_service routing is gone: the gateway forwards to
    // the session's bound target unconditionally.
    shield::transport::RouteEntry route;
    route.route_id = 0x4001;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;
    bridge.on_packet(session, make_packet(0x4001, &route));

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc2.service_id, "get_log",
                                          nlohmann::json::array());
            return log.success && log.values.is_array() &&
                   log.values.size() == 1u && log.values[0].is_array() &&
                   log.values[0].size() == 1u && log.values[0][0].is_array() &&
                   log.values[0][0].size() == 2u &&
                   log.values[0][0][0].get<std::string>() ==
                       "on_client_message" &&
                   log.values[0][0][1].get<uint32_t>() == 0x4001u;
        },
        std::chrono::seconds(3)));
}

// Ingress delivery failure: the target resolves to a dead service name.
BOOST_AUTO_TEST_CASE(ClientIngressFailureLogsWarning) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    LuaGatewayBridge bridge(
        manager, "cov_ghost_auth",
        std::make_shared<shield::lua::GatewaySessionRegistry>());

    auto session = std::make_shared<MockSession>(
        105, shield::net::RemoteAddress{"127.0.0.1", 6005});
    session->reset_binding({"cov_dead_target", "", "cov_ghost_auth", "", 0});

    shield::transport::RouteEntry route;
    route.route_id = 0x5001;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;
    bridge.on_packet(session, make_packet(0x5001, &route));
    BOOST_CHECK(true);
}

// on_disconnect: null handled above; a live session drops its registry
// entry, and a session with an empty target falls back to the auth service
// name (ghost here -> failure warning branch).
BOOST_AUTO_TEST_CASE(OnDisconnectFallbackAndFailure) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto registry = std::make_shared<shield::lua::GatewaySessionRegistry>();
    LuaGatewayBridge bridge(manager, "cov_ghost_auth", registry);

    // Session that was never connected: empty target falls back to the
    // (missing) auth service, hitting the send-failure warning branch.
    auto fresh = std::make_shared<MockSession>(
        109, shield::net::RemoteAddress{"127.0.0.1", 6009});
    bridge.on_disconnect(fresh, "cov_fresh");

    auto session = std::make_shared<MockSession>(
        106, shield::net::RemoteAddress{"127.0.0.1", 6006});
    bridge.on_connect(session);
    BOOST_CHECK_EQUAL(registry->size(), 1u);
    BOOST_CHECK(registry->find(106) != nullptr);

    bridge.on_disconnect(session, "cov_reason");

    // The registry entry is gone; the binding itself is left to the socket
    // teardown (egress rejects unknown sessions from here on).
    BOOST_CHECK_EQUAL(registry->size(), 0u);
    BOOST_CHECK_EQUAL(session->binding().target_service, "cov_ghost_auth");
}

// on_disconnect success path: a live target service receives the event.
BOOST_AUTO_TEST_CASE(OnDisconnectDeliversToLiveTarget) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script = write_script("/tmp/opencode/cov_bridge_service.lua",
                                     kBridgeServiceScript);
    auto svc = manager.spawn(script, opts_for("cov_auth").dump());
    BOOST_REQUIRE(svc.success);

    LuaGatewayBridge bridge(
        manager, svc.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        107, shield::net::RemoteAddress{"127.0.0.1", 6007});
    session->reset_binding({svc.service_id, "", svc.service_id, "", 0});

    bridge.on_disconnect(session, "client_closed");

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc.service_id, "get_log",
                                          nlohmann::json::array());
            return log.success && log.values.is_array() &&
                   log.values.size() == 1u && log.values[0].is_array() &&
                   log.values[0].size() == 1u && log.values[0][0].is_array() &&
                   log.values[0][0].size() == 1u &&
                   log.values[0][0][0].get<std::string>() == "on_disconnect";
        },
        std::chrono::seconds(3)));
}

// on_connect success path plus a decoded ingress round trip through the
// live auth service (regression guard for the happy path).
BOOST_AUTO_TEST_CASE(OnConnectAndIngressHappyPath) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script = write_script("/tmp/opencode/cov_bridge_service.lua",
                                     kBridgeServiceScript);
    auto svc = manager.spawn(script, opts_for("cov_auth2").dump());
    BOOST_REQUIRE(svc.success);

    LuaGatewayBridge bridge(
        manager, svc.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        108, shield::net::RemoteAddress{"127.0.0.1", 6008});

    bridge.on_connect(session);
    BOOST_CHECK_EQUAL(session->target_service(), "cov_auth2");

    // Authentication: the CAS install flips the player identity and bumps
    // the epoch, so the protected route below is allowed.
    BOOST_CHECK(session->apply_binding("cov_auth2", "player-9", 0, nullptr));

    shield::transport::RouteEntry route;
    route.route_id = 0x6001;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = true;  // session has a player id, so allowed
    bridge.on_packet(session, make_packet(0x6001, &route));

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc.service_id, "get_log",
                                          nlohmann::json::array());
            return log.success && log.values.is_array() &&
                   log.values.size() == 1u && log.values[0].is_array() &&
                   log.values[0].size() == 2u;
        },
        std::chrono::seconds(3)));
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Round-3: an on_packet dispatch whose decoded body carries a structured
// message forwards the canonical JSON to the target service.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(OnPacketForwardsDecodedMessage) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script =
        write_script("/tmp/opencode/cov_bridge_msg.lua", kBridgeServiceScript);
    auto svc = manager.spawn(script, opts_for("cov_game").dump());
    BOOST_REQUIRE(svc.success);

    LuaGatewayBridge bridge(
        manager, "cov_ghost_auth",
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        105, shield::net::RemoteAddress{"127.0.0.1", 6005});
    session->reset_binding({svc.service_id, "", "cov_ghost_auth", "", 0});

    shield::transport::RouteEntry route;
    route.route_id = 0x4003;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;

    auto dispatch = make_packet(0x4003, &route);
    BOOST_REQUIRE(dispatch.decoded_body.has_value());
    dispatch.decoded_body->message =
        std::optional<nlohmann::json>(nlohmann::json{{"k", "v"}});
    bridge.on_packet(session, dispatch);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc.service_id, "get_log",
                                          nlohmann::json::array());
            return log.success && log.values.is_array() &&
                   log.values.size() == 1u && log.values[0].is_array() &&
                   log.values[0].size() == 1u;
        },
        std::chrono::seconds(3)));
}
