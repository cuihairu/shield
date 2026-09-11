// Coverage tests for src/lua/lua_gateway_bridge.cpp (M3 typed dispatch).
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

// M3-contract service: typed control messages land in on_client_bound /
// on_disconnect / on_client_unbound; ingress lands in the compiled c2s
// bindings declared in the spawn opts. Every entry is appended to `log` so
// the tests can assert on what actually reached the VM.
const char* kBridgeServiceScript = R"lua(
local M = {}
local log = {}
local function key_of(client)
    if type(client) == "userdata" then
        local ok, id = pcall(client.session_id, client)
        if ok then return id end
    elseif type(client) == "table" then
        return client.session_id
    end
    return nil
end
function M.on_init(args) end
function M.on_client_bound(ctx, client)
  table.insert(log, {"bound", key_of(client)}) return true
end
function M.on_disconnect(ctx, client, reason)
  table.insert(log, {"disconnected", reason}) return true
end
function M.on_client_unbound(ctx, client, reason)
  table.insert(log, {"unbound", reason}) return true
end
function M.h_move(ctx, client, request)
  table.insert(log, {"move", key_of(client), request}) return true
end
function M.get_log(ctx) return log end
return M
)lua";

std::string write_script(const std::string& path, const char* content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
    return path;
}

// Two c2s routes: 0x4001 open, 0x6001 auth-required. Both bind to h_move.
nlohmann::json opts_for(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
        {"rpc",
         {{"routes", nlohmann::json::array({
                         nlohmann::json{{"id", 0x4001},
                                        {"name", "move_open"},
                                        {"direction", "c2s"},
                                        {"binding", "h_move"},
                                        {"requires_auth", false}},
                         nlohmann::json{{"id", 0x6001},
                                        {"name", "move_auth"},
                                        {"direction", "c2s"},
                                        {"binding", "h_move"},
                                        {"requires_auth", true}},
                     })}}},
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

// Scan helper: the anon_send mailbox does not guarantee an ordering between
// control and ingress messages observable from the test thread, so asserts
// look for the expected entry anywhere in the log.
inline const nlohmann::json* find_entry(const nlohmann::json& log,
                                        const std::string& kind) {
    for (const auto& entry : log) {
        if (entry.is_array() && !entry.empty() &&
            entry[0].get<std::string>() == kind) {
            return &entry;
        }
    }
    return nullptr;
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

// on_connect against a missing auth service: the initial binding is
// installed and the session is registered with the gateway registry, and the
// missing-actor warning branch fires.
BOOST_AUTO_TEST_CASE(OnConnectGhostAuthLogsWarning) {
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

// A null registry skips registration (both on_connect and on_disconnect)
// without disturbing the binding install or the missing-actor warning.
BOOST_AUTO_TEST_CASE(NullRegistrySkipsRegistration) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    LuaGatewayBridge bridge(manager, "cov_ghost_auth", nullptr);

    auto session = std::make_shared<MockSession>(
        111, shield::net::RemoteAddress{"127.0.0.1", 6011});
    bridge.on_connect(session);
    BOOST_CHECK_EQUAL(session->binding().target_service, "cov_ghost_auth");

    bridge.on_disconnect(session, "cov_null_registry");
    BOOST_CHECK(true);
}

// on_packet rejection branches: not-ok packet, drop, forward-raw, unknown
// route, wrong direction, unauthenticated access to a protected route.
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

    BOOST_CHECK(true);
}

// A valid route on a session whose binding carries no target service hits
// the empty-target warning branch (single target: no fallback exists).
BOOST_AUTO_TEST_CASE(OnPacketEmptyTargetWarning) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    LuaGatewayBridge bridge(
        manager, "cov_ghost_auth",
        std::make_shared<shield::lua::GatewaySessionRegistry>());

    // Never on_connect-ed: default binding has an empty target_service.
    auto session = std::make_shared<MockSession>(
        103, shield::net::RemoteAddress{"127.0.0.1", 6003});

    shield::transport::RouteEntry route;
    route.route_id = 0x3001;
    route.direction = shield::transport::RouteDirection::Bidirectional;
    route.requires_auth = false;
    bridge.on_packet(session, make_packet(0x3001, &route));
    BOOST_CHECK(true);
}

// The bound target resolves to a service name with no actor: the delivery
// failure warning branch fires.
BOOST_AUTO_TEST_CASE(ClientIngressTargetActorMissing) {
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

// Happy path end to end: on_connect delivers Bound to the live auth service,
// authenticated ingress (decoded body carrying a canonical JSON message)
// reaches the compiled binding, and on_disconnect delivers Disconnected.
BOOST_AUTO_TEST_CASE(OnConnectIngressAndDisconnectHappyPath) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script = write_script("/tmp/opencode/cov_bridge_service.lua",
                                     kBridgeServiceScript);
    auto svc = manager.spawn(script, opts_for("cov_auth").dump());
    BOOST_REQUIRE(svc.success);

    auto registry = std::make_shared<shield::lua::GatewaySessionRegistry>();
    LuaGatewayBridge bridge(manager, svc.service_id, registry);
    auto session = std::make_shared<MockSession>(
        107, shield::net::RemoteAddress{"127.0.0.1", 6007});

    bridge.on_connect(session);
    BOOST_CHECK_EQUAL(session->target_service(), svc.service_id);
    BOOST_CHECK_EQUAL(registry->size(), 1u);

    // Authentication: the CAS install flips the player identity and bumps
    // the epoch, so the protected route below is allowed.
    BOOST_CHECK(session->apply_binding(svc.service_id, "player-9", 0, nullptr));

    shield::transport::RouteEntry route;
    route.route_id = 0x6001;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = true;  // session has a player id, so allowed

    auto dispatch = make_packet(0x6001, &route);
    BOOST_REQUIRE(dispatch.decoded_body.has_value());
    dispatch.decoded_body->message =
        std::optional<nlohmann::json>(nlohmann::json{{"k", "v"}});
    bridge.on_packet(session, dispatch);

    bridge.on_disconnect(session, "client_closed");
    BOOST_CHECK_EQUAL(registry->size(), 0u);

    // log = {{"bound", 107}, {"move", 107, {"k":"v"}}, {"disconnected",
    // "client_closed"}} in some order -- the full typed lifecycle.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc.service_id, "get_log",
                                          nlohmann::json::array());
            if (!log.success || !log.values.is_array() ||
                log.values.size() != 1u || !log.values[0].is_array() ||
                log.values[0].size() != 3u) {
                return false;
            }
            const auto& entries = log.values[0];
            const nlohmann::json* bound = find_entry(entries, "bound");
            const nlohmann::json* move = find_entry(entries, "move");
            const nlohmann::json* disc = find_entry(entries, "disconnected");
            return bound != nullptr && (*bound)[1].get<uint64_t>() == 107u &&
                   move != nullptr && (*move)[1].get<uint64_t>() == 107u &&
                   (*move)[2]["k"].get<std::string>() == "v" &&
                   disc != nullptr &&
                   (*disc)[1].get<std::string>() == "client_closed";
        },
        std::chrono::seconds(3)));
}

// No decoded body: body bytes come straight off the wire packet; a
// non-JSON payload reaches the handler as the raw string fallback.
BOOST_AUTO_TEST_CASE(OnPacketWithoutDecodedBodyUsesWireBytes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script =
        write_script("/tmp/opencode/cov_bridge_wire.lua", kBridgeServiceScript);
    auto svc = manager.spawn(script, opts_for("cov_wire").dump());
    BOOST_REQUIRE(svc.success);

    LuaGatewayBridge bridge(
        manager, svc.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        108, shield::net::RemoteAddress{"127.0.0.1", 6008});
    bridge.on_connect(session);

    shield::transport::RouteEntry route;
    route.route_id = 0x4001;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;
    bridge.on_packet(session, make_packet(0x4001, &route, false));

    // JSON-parse of "body" fails -> the descriptor contract's string
    // fallback form. Both the Bound control message and the ingress land in
    // the log (order not observed from the test thread).
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc.service_id, "get_log",
                                          nlohmann::json::array());
            if (!log.success || !log.values.is_array() ||
                log.values.size() != 1u || !log.values[0].is_array() ||
                log.values[0].size() != 2u) {
                return false;
            }
            const nlohmann::json* move = find_entry(log.values[0], "move");
            return move != nullptr && (*move)[2].get<std::string>() == "body";
        },
        std::chrono::seconds(3)));
}

// Decoded body without a codec message: bytes still come from the decoded
// body, and a JSON-decodable payload is parsed into the request table.
BOOST_AUTO_TEST_CASE(OnPacketDecodedBodyWithoutMessageParsesJson) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const auto script = write_script("/tmp/opencode/cov_bridge_decoded.lua",
                                     kBridgeServiceScript);
    auto svc = manager.spawn(script, opts_for("cov_decoded").dump());
    BOOST_REQUIRE(svc.success);

    LuaGatewayBridge bridge(
        manager, svc.service_id,
        std::make_shared<shield::lua::GatewaySessionRegistry>());
    auto session = std::make_shared<MockSession>(
        109, shield::net::RemoteAddress{"127.0.0.1", 6009});
    bridge.on_connect(session);

    shield::transport::RouteEntry route;
    route.route_id = 0x4001;
    route.direction = shield::transport::RouteDirection::ClientToServer;
    route.requires_auth = false;

    auto dispatch = make_packet(0x4001, &route);
    dispatch.decoded_body->bytes = std::vector<std::uint8_t>{
        '{', '"', 'w', 'i', 'r', 'e', '"', ':', '1', '}'};
    bridge.on_packet(session, dispatch);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc.service_id, "get_log",
                                          nlohmann::json::array());
            if (!log.success || !log.values.is_array() ||
                log.values.size() != 1u || !log.values[0].is_array() ||
                log.values[0].size() != 2u) {
                return false;
            }
            const nlohmann::json* move = find_entry(log.values[0], "move");
            return move != nullptr && (*move)[2]["wire"].get<int>() == 1;
        },
        std::chrono::seconds(3)));
}

// on_disconnect for a session with an empty binding returns early (the
// gateway actor already delivered Unbound on a kick).
BOOST_AUTO_TEST_CASE(OnDisconnectEmptyBindingSkipsNotify) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    auto registry = std::make_shared<shield::lua::GatewaySessionRegistry>();
    LuaGatewayBridge bridge(manager, "cov_ghost_auth", registry);

    // Never connected: empty binding, but the registry remove still runs.
    auto fresh = std::make_shared<MockSession>(
        110, shield::net::RemoteAddress{"127.0.0.1", 6010});
    bridge.on_disconnect(fresh, "cov_fresh");
    BOOST_CHECK_EQUAL(registry->size(), 0u);
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()
