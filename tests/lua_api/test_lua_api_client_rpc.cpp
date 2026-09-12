// LAPI client_rpc: closed-loop tests for shield.client.bind / close and the
// generated shield.client_rpc.<name> egress helpers. Each case wires a real
// gateway actor (spawn_gateway_actor) to a spawned service VM, so the bind
// round-trip exercises the exact production path: handler coroutine ->
// suspend_for_call -> ClientBindRequest -> gateway CAS -> complete_call ->
// resume with a fresh ClientRef.
#define BOOST_TEST_MODULE LuaApiClientRpcTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/rpc_descriptor.hpp"

using namespace shield::lua;

namespace {

const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json client_marker(const std::string& gateway, uint64_t session_id,
                             uint32_t epoch, const std::string& player_id) {
    return ClientContextData{gateway, session_id, epoch, player_id, "json"}
        .to_json();
}

// The auth service's own routes: one s2c helper compiled into its VM at
// spawn; the gateway side validates egress against the same route set.
nlohmann::json auth_opts(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
        {"rpc",
         {{"routes", nlohmann::json::array({nlohmann::json{
                         {"id", 1001},
                         {"name", "login_result"},
                         {"direction", "s2c"},
                         {"binding", "login_result"},
                         {"requires_auth", false},
                     }})}}},
    };
}

shield::transport::RpcDescriptorTable gateway_routes() {
    shield::transport::RpcDescriptor descriptor;
    descriptor.route_id = 1001;
    descriptor.name = "login_result";
    descriptor.direction = shield::transport::RouteDirection::ServerToClient;
    descriptor.requires_auth = false;
    descriptor.binding = "login_result";
    shield::transport::RpcDescriptorTable table;
    table.add(descriptor);
    return table;
}

// Spawn opts for the ingress-driven case: the auth VM also owns the c2s
// login binding, so a typed ClientIngress can reach M.login through the
// spawn-time RPC table exactly as the gateway bridge would deliver it.
nlohmann::json ingress_auth_opts(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
        {"rpc",
         {{"routes", nlohmann::json::array({nlohmann::json{
                                                {"id", 1000},
                                                {"name", "login"},
                                                {"direction", "c2s"},
                                                {"binding", "login"},
                                                {"requires_auth", false},
                                            },
                                            nlohmann::json{
                                                {"id", 1001},
                                                {"name", "login_result"},
                                                {"direction", "s2c"},
                                                {"binding", "login_result"},
                                                {"requires_auth", false},
                                            }})}}},
    };
}

class MockSession final : public shield::net::Session {
public:
    MockSession(shield::net::SessionId id, shield::net::RemoteAddress remote)
        : id_(id), remote_(std::move(remote)) {}

    shield::net::SessionId id() const override { return id_; }
    shield::net::RemoteAddress remote_addr() const override { return remote_; }
    bool send(const std::vector<uint8_t>&, std::string*) override {
        return alive_;
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
                      std::string*) override {
        sent_messages_.push_back(message);
        return true;
    }
    void set_user_data(std::string, std::string) override {}
    std::string get_user_data(std::string_view) const override { return ""; }

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

    // Test-side read helpers (not part of the interface).
    uint32_t epoch() const { return binding_.epoch; }
    std::string target_service() const { return binding_.target_service; }
    const std::string& close_reason() const { return close_reason_; }
    const std::vector<shield::transport::DecodedBody>& sent_messages() const {
        return sent_messages_;
    }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool alive_ = true;
    std::string close_reason_;
    std::vector<shield::transport::DecodedBody> sent_messages_;
    std::unordered_map<std::string, std::string> user_data_;
    shield::net::SessionBinding binding_;
};

std::shared_ptr<MockSession> connect_session(GatewaySessionRegistry& registry,
                                             shield::net::SessionId id) {
    auto session = std::make_shared<MockSession>(
        id, shield::net::RemoteAddress{"127.0.0.1", 40000});
    shield::net::SessionBinding initial;
    initial.target_service = "auth";
    initial.gateway_name = "gw";
    initial.protocol_profile_id = "json";
    session->reset_binding(initial);
    registry.add(session, session->binding());
    return session;
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

struct GatewayWorld {
    caf::actor_system_config cfg;
    caf::actor_system system;
    LuaRuntime runtime;
    LuaServiceManager manager;
    std::shared_ptr<GatewaySessionRegistry> registry;
    std::shared_ptr<GatewayStats> stats;
    caf::actor gateway;

    GatewayWorld()
        : system(cfg),
          manager(runtime, system),
          registry(std::make_shared<GatewaySessionRegistry>()),
          stats(std::make_shared<GatewayStats>()) {
        GatewayDeps deps{"gw", registry, stats, gateway_routes(), &manager};
        gateway = spawn_gateway_actor(system, std::move(deps));
        manager.register_gateway_actor("gw", gateway);
    }
};

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(ClientRpcTests)

BOOST_AUTO_TEST_CASE(BindClosedLoopBindsAndEgresses) {
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    auth_opts("auth_loop").dump());
    BOOST_REQUIRE(auth.success);

    auto session = connect_session(*world.registry, 101);

    CallResult result = world.manager.call(
        auth.service_id, "login",
        nlohmann::json::array({client_marker("gw", 101, 0, ""), "player-1"}));
    BOOST_REQUIRE(result.success);
    BOOST_REQUIRE_EQUAL(result.values.size(), 1u);
    const nlohmann::json& value = result.values[0];

    // The handler resumed with a fresh ClientRef: new epoch, new player,
    // and the s2c helper pushed the welcome payload through the gateway.
    BOOST_CHECK_EQUAL(value["bound"].get<bool>(), true);
    BOOST_CHECK_EQUAL(value["player"].get<std::string>(), "player-1");
    BOOST_CHECK_EQUAL(value["epoch"].get<uint32_t>(), 1u);
    BOOST_CHECK_EQUAL(value["session"].get<uint64_t>(), 101u);
    BOOST_CHECK_EQUAL(value["sent"].get<bool>(), true);

    // The session's single target was swapped atomically, and the egress
    // arrived on the socket with the descriptor route id and route name.
    BOOST_CHECK_EQUAL(session->target_service(), "player");
    BOOST_CHECK_EQUAL(session->binding().player_id, "player-1");
    BOOST_CHECK_EQUAL(session->epoch(), 1u);
    BOOST_CHECK(wait_until(
        [&]() {
            return !session->sent_messages().empty() &&
                   world.stats->egress_accepted.load() == 1u;
        },
        std::chrono::seconds(2)));
    BOOST_REQUIRE_EQUAL(session->sent_messages().size(), 1u);
    BOOST_REQUIRE(session->sent_messages()[0].has_message());
    BOOST_CHECK_EQUAL(
        (*session->sent_messages()[0].message)["welcome"].get<std::string>(),
        "player-1");
    BOOST_CHECK_EQUAL(session->sent_messages()[0].route_id, 1001u);
    BOOST_CHECK_EQUAL(session->sent_messages()[0].route_name, "login_result");
    BOOST_CHECK_EQUAL(world.stats->binds_ok.load(), 1u);
}

BOOST_AUTO_TEST_CASE(StaleReferenceFailsWithEpochExpired) {
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    auth_opts("auth_stale").dump());
    BOOST_REQUIRE(auth.success);

    auto session = connect_session(*world.registry, 102);
    CallResult first = world.manager.call(
        auth.service_id, "login",
        nlohmann::json::array({client_marker("gw", 102, 0, ""), "player-1"}));
    BOOST_REQUIRE(first.success);
    BOOST_CHECK_EQUAL(first.values[0]["bound"].get<bool>(), true);

    // Replay the pre-login marker: its epoch no longer matches the live
    // binding, so the CAS fails and the handler sees the stable error code.
    CallResult stale = world.manager.call(
        auth.service_id, "login",
        nlohmann::json::array({client_marker("gw", 102, 0, ""), "player-2"}));
    BOOST_REQUIRE(stale.success);
    BOOST_CHECK_EQUAL(stale.values[0]["bound"].get<bool>(), false);
    BOOST_CHECK_EQUAL(stale.values[0]["code"].get<std::string>(),
                      "client_rpc.epoch_expired");

    // The live binding is untouched by the rejected bind.
    BOOST_CHECK_EQUAL(session->binding().player_id, "player-1");
    BOOST_CHECK_EQUAL(session->epoch(), 1u);
    BOOST_CHECK_EQUAL(world.stats->binds_epoch_expired.load(), 1u);
}

BOOST_AUTO_TEST_CASE(BindWithoutGatewayCompletesWithFailure) {
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    auth_opts("auth_ghost").dump());
    BOOST_REQUIRE(auth.success);

    // The marker points at a gateway that has no registered actor: the
    // primitive suspends, then completes the waiter with the stable error.
    CallResult ghost = world.manager.call(
        auth.service_id, "login",
        nlohmann::json::array(
            {client_marker("nowhere", 103, 0, ""), "player-9"}));
    BOOST_REQUIRE(ghost.success);
    BOOST_CHECK_EQUAL(ghost.values[0]["bound"].get<bool>(), false);
    BOOST_CHECK_EQUAL(ghost.values[0]["code"].get<std::string>(),
                      "client_rpc.epoch_expired");
    // The request never reached a gateway actor, so gateway-side bind
    // counters stay flat; the primitive completed the waiter itself.
    BOOST_CHECK_EQUAL(world.stats->binds_ok.load(), 0u);
    BOOST_CHECK_EQUAL(world.stats->binds_epoch_expired.load(), 0u);
}

BOOST_AUTO_TEST_CASE(CloseGoesThroughGatewayAndKillsSession) {
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    auth_opts("auth_close").dump());
    BOOST_REQUIRE(auth.success);

    auto session = connect_session(*world.registry, 104);
    CallResult login = world.manager.call(
        auth.service_id, "login",
        nlohmann::json::array({client_marker("gw", 104, 0, ""), "player-4"}));
    BOOST_REQUIRE(login.success);
    BOOST_CHECK_EQUAL(login.values[0]["bound"].get<bool>(), true);

    CallResult kick = world.manager.call(
        auth.service_id, "kick",
        nlohmann::json::array(
            {client_marker("gw", 104, 1, "player-4"), "ban"}));
    BOOST_REQUIRE(kick.success);
    BOOST_CHECK_EQUAL(kick.values[0].get<bool>(), true);

    // The gateway invalidated the binding, dropped the registry entry, and
    // closed the socket with the requested reason.
    BOOST_CHECK(wait_until(
        [&]() { return !session->is_alive() && world.registry->size() == 0u; },
        std::chrono::seconds(2)));
    BOOST_CHECK_EQUAL(session->close_reason(), "ban");
    BOOST_CHECK_EQUAL(world.stats->close_requests.load(), 1u);
}

BOOST_AUTO_TEST_CASE(EgressRejectsUnknownGatewayAndInvalidClient) {
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    auth_opts("auth_egress").dump());
    BOOST_REQUIRE(auth.success);

    // Valid marker, but the gateway name has no registered actor.
    CallResult ghost_egress = world.manager.call(
        auth.service_id, "egress",
        nlohmann::json::array(
            {client_marker("nowhere", 105, 0, ""), nlohmann::json::object()}));
    BOOST_REQUIRE(ghost_egress.success);
    BOOST_CHECK_EQUAL(ghost_egress.values[0].get<bool>(), false);

    // Not a client identity at all: the helper refuses without sending.
    CallResult invalid_egress =
        world.manager.call(auth.service_id, "egress",
                           nlohmann::json::array({nlohmann::json::object(),
                                                  nlohmann::json::object()}));
    BOOST_REQUIRE(invalid_egress.success);
    BOOST_CHECK_EQUAL(invalid_egress.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(world.stats->egress_accepted.load(), 0u);
}

BOOST_AUTO_TEST_CASE(EgressHelperAndPrimitiveAcceptTableStringRefuseOthers) {
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    auth_opts("auth_payloads").dump());
    BOOST_REQUIRE(auth.success);

    auto session = connect_session(*world.registry, 106);

    // String payload through the helper: raw bytes onto the wire.
    CallResult raw = world.manager.call(
        auth.service_id, "egress_raw",
        nlohmann::json::array({client_marker("gw", 106, 0, "")}));
    BOOST_REQUIRE(raw.success);
    BOOST_CHECK_EQUAL(raw.values[0].get<bool>(), true);

    // Payload that is neither table nor string: refused without sending.
    CallResult bad_payload = world.manager.call(
        auth.service_id, "egress_bad_payload",
        nlohmann::json::array({client_marker("gw", 106, 0, "")}));
    BOOST_REQUIRE(bad_payload.success);
    BOOST_CHECK_EQUAL(bad_payload.values[0].get<bool>(), false);

    // The raw primitive takes an explicit route id: table and string forms.
    CallResult primitive_table = world.manager.call(
        auth.service_id, "primitive_egress",
        nlohmann::json::array({client_marker("gw", 106, 0, ""),
                               nlohmann::json::object({{"ok", true}})}));
    BOOST_REQUIRE(primitive_table.success);
    BOOST_CHECK_EQUAL(primitive_table.values[0].get<bool>(), true);

    CallResult primitive_raw = world.manager.call(
        auth.service_id, "primitive_egress",
        nlohmann::json::array({client_marker("gw", 106, 0, ""), "bytes"}));
    BOOST_REQUIRE(primitive_raw.success);
    BOOST_CHECK_EQUAL(primitive_raw.values[0].get<bool>(), true);

    // The primitive refuses non-table non-string payloads just like the
    // helper.
    CallResult primitive_bad = world.manager.call(
        auth.service_id, "primitive_egress",
        nlohmann::json::array({client_marker("gw", 106, 0, ""), 42}));
    BOOST_REQUIRE(primitive_bad.success);
    BOOST_CHECK_EQUAL(primitive_bad.values[0].get<bool>(), false);

    // The gateway accepted the three valid pushes and dropped the refused
    // one (it never reached the gateway).
    BOOST_CHECK(
        wait_until([&]() { return world.stats->egress_accepted.load() == 3u; },
                   std::chrono::seconds(2)));
    BOOST_REQUIRE_EQUAL(session->sent_messages().size(), 3u);
    BOOST_CHECK(
        (session->sent_messages()[0].bytes ==
         std::vector<uint8_t>{'r', 'a', 'w', '-', 'b', 'y', 't', 'e', 's'}));
    BOOST_REQUIRE(session->sent_messages()[1].has_message());
    BOOST_CHECK_EQUAL((*session->sent_messages()[1].message)["ok"].get<bool>(),
                      true);
    BOOST_CHECK((session->sent_messages()[2].bytes ==
                 std::vector<uint8_t>{'b', 'y', 't', 'e', 's'}));
}

BOOST_AUTO_TEST_CASE(BindRejectsInvalidClientArgument) {
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    auth_opts("auth_bad_bind").dump());
    BOOST_REQUIRE(auth.success);

    // bind with a non-identity argument fails fast with the stable error
    // code; the wrapper never yields for this path.
    CallResult bad =
        world.manager.call(auth.service_id, "bad_bind",
                           nlohmann::json::array({nlohmann::json::object()}));
    BOOST_REQUIRE(bad.success);
    BOOST_CHECK_EQUAL(bad.values[0]["ok"].get<bool>(), false);
    BOOST_CHECK_EQUAL(bad.values[0]["code"].get<std::string>(),
                      "invalid_client_reference");
    BOOST_CHECK_EQUAL(world.stats->binds_ok.load(), 0u);
}

BOOST_AUTO_TEST_CASE(IngressDrivenBindResumesHandlerCoroutine) {
    // Regression (M6 e2e): dispatch_client_ingress must establish the
    // dispatch context before running the handler coroutine. Without it a
    // shield.client.bind inside a client RPC handler records an empty
    // caller_service, the gateway's completion cannot route back to this
    // actor, and the handler coroutine never resumes — no egress, no
    // error, the session just hangs.
    GatewayWorld world;
    auto auth = world.manager.spawn(TEST_SCRIPTS_DIR + "client_rpc_service.lua",
                                    ingress_auth_opts("auth_ingress").dump());
    BOOST_REQUIRE(auth.success);

    auto session = connect_session(*world.registry, 103);

    ClientIngress ingress;
    ingress.context = ClientContextData{"gw", 103, 0, "", "json"};
    ingress.route_id = 1000;
    ingress.decoded_request = nlohmann::json("player-1");

    caf::anon_send(world.manager.service_actor(auth.service_id), ingress);

    // The handler resumed after the bind and pushed the welcome egress.
    BOOST_CHECK(wait_until(
        [&]() {
            return !session->sent_messages().empty() &&
                   world.stats->egress_accepted.load() == 1u;
        },
        std::chrono::seconds(2)));
    BOOST_CHECK_EQUAL(session->target_service(), "player");
    BOOST_CHECK_EQUAL(session->epoch(), 1u);
    BOOST_REQUIRE_EQUAL(session->sent_messages().size(), 1u);
    BOOST_REQUIRE(session->sent_messages()[0].has_message());
    BOOST_CHECK_EQUAL(
        (*session->sent_messages()[0].message)["welcome"].get<std::string>(),
        "player-1");
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace
