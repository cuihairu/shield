// LAPI-011: shield_player P0 — setup hook validation, the PlayerManager
// primitives, PlayerRef materialization, the client-message guard, and the
// authenticate closed loop through a real gateway actor (spawn + bind +
// Bound -> login/ready), including the offline queue and the
// reconnect-window restore.
//
// When SHIELD_ENABLE_PLAYER is off, only the stub cases compile: every
// shield.player.* entry reports module_unavailable instead of being nil.
#define BOOST_TEST_MODULE LuaApiPlayerTests
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
#include <utility>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/core/service_message.hpp"
#include "shield/lua/gateway_actor.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/rpc_descriptor.hpp"

using namespace shield::lua;

namespace {

const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
const std::string PLAYER_SCRIPT = TEST_SCRIPTS_DIR + "player_service.lua";

nlohmann::json client_marker(const std::string& gateway, uint64_t session_id,
                             uint32_t epoch, const std::string& player_id) {
    return ClientContextData{gateway, session_id, epoch, player_id, "json"}
        .to_json();
}

// Routes shared by the auth-entry VM and (through opts.instance_routes) the
// spawned player instances: one s2c route for push tests.
nlohmann::json test_routes() {
    return nlohmann::json::array({nlohmann::json{
        {"id", 1001},
        {"name", "login_result"},
        {"direction", "s2c"},
        {"binding", "login_result"},
        {"requires_auth", false},
    }});
}

nlohmann::json service_opts(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
        {"rpc", {{"routes", test_routes()}}},
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
    std::vector<shield::transport::DecodedBody>& sent_messages() {
        return sent_messages_;
    }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool alive_ = true;
    std::string close_reason_;
    std::vector<shield::transport::DecodedBody> sent_messages_;
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

CallResult call(LuaServiceManager& manager, const std::string& service,
                const std::string& method, nlohmann::json args) {
    return manager.call(service, method, std::move(args), 5000);
}

// Read the per-VM hook call log from a spawned player_service VM.
std::vector<nlohmann::json> calls(LuaServiceManager& manager,
                                  const std::string& service) {
    CallResult cr =
        call(manager, service, "calls_json", nlohmann::json::array());
    std::vector<nlohmann::json> out;
    if (cr.success && !cr.values.empty() && cr.values[0].is_array()) {
        for (const auto& entry : cr.values[0]) {
            out.push_back(entry);
        }
    }
    return out;
}

}  // namespace

#ifdef SHIELD_ENABLE_PLAYER

#include "shield/player/player_manager.hpp"

using shield::player::PlayerConfig;
using shield::player::PlayerManager;
using shield::player::SessionState;

namespace {

// Process-wide PlayerManager for the whole module: a small queue limit so
// the offline-queue case can fill it cheaply; the other cases never reach
// it.
struct PlayerGlobalFixture {
    PlayerConfig config;
    std::unique_ptr<PlayerManager> manager;

    PlayerGlobalFixture() {
        initialize_caf_types();
        config.enabled = true;
        config.message_queue_limit = 8;
        manager = std::make_unique<PlayerManager>(config);
        PlayerManager::set_global(manager.get());
    }
    ~PlayerGlobalFixture() { PlayerManager::set_global(nullptr); }
};
BOOST_GLOBAL_FIXTURE(PlayerGlobalFixture);

}  // namespace

BOOST_AUTO_TEST_SUITE(Lapi011Setup)

BOOST_AUTO_TEST_CASE(LAPI_011_01_MissingRequiredHookFails) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    for (const std::string& hook :
         {"auth", "login", "client_message", "disconnect", "logout"}) {
        auto svc = manager.spawn(PLAYER_SCRIPT,
                                 service_opts("auth_missing_" + hook).dump());
        BOOST_REQUIRE(svc.success);
        CallResult cr =
            call(manager, svc.service_id, "do_setup",
                 nlohmann::json::array({"missing_" + hook, nullptr, nullptr}));
        BOOST_REQUIRE(cr.success);
        BOOST_CHECK_EQUAL(cr.values[0]["ok"].get<bool>(), false);
        BOOST_CHECK_EQUAL(cr.values[0]["code"].get<std::string>(),
                          "setup_invalid");
    }
}

BOOST_AUTO_TEST_CASE(LAPI_011_02_SetupReturnsFacade) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_facade").dump());
    BOOST_REQUIRE(svc.success);
    CallResult cr =
        call(manager, svc.service_id, "do_setup",
             nlohmann::json::array({"full", nullptr, test_routes()}));
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0]["ok"].get<bool>(), true);
    BOOST_CHECK_EQUAL(cr.values[0]["has_auth"].get<bool>(), true);
    BOOST_CHECK_EQUAL(cr.values[0]["has_push"].get<bool>(), true);
    BOOST_CHECK_EQUAL(cr.values[0]["has_session"].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(LAPI_011_03_SetupAcceptsModuleMethodNames) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_names").dump());
    BOOST_REQUIRE(svc.success);
    CallResult cr = call(manager, svc.service_id, "do_setup",
                         nlohmann::json::array({"names", nullptr, nullptr}));
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0]["ok"].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(LAPI_011_04_SetupTwiceFails) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_twice").dump());
    BOOST_REQUIRE(svc.success);
    CallResult first = call(manager, svc.service_id, "do_setup",
                            nlohmann::json::array({"full", nullptr, nullptr}));
    BOOST_REQUIRE(first.success);
    BOOST_CHECK_EQUAL(first.values[0]["ok"].get<bool>(), true);

    CallResult second = call(manager, svc.service_id, "do_setup",
                             nlohmann::json::array({"full", nullptr, nullptr}));
    BOOST_REQUIRE(second.success);
    BOOST_CHECK_EQUAL(second.values[0]["ok"].get<bool>(), false);
    BOOST_CHECK_EQUAL(second.values[0]["code"].get<std::string>(),
                      "setup_invalid");
}

BOOST_AUTO_TEST_CASE(LAPI_011_05_DefaultsExposeFourImplementations) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_def").dump());
    BOOST_REQUIRE(svc.success);
    CallResult names = call(manager, svc.service_id, "defaults_names",
                            nlohmann::json::array());
    BOOST_REQUIRE(names.success);
    // allow/ready/reconnect/save (sorted): every optional hook has a real
    // default implementation (never a silent noop).
    BOOST_REQUIRE_EQUAL(names.values[0].size(), 4u);
    BOOST_CHECK_EQUAL(names.values[0][0].get<std::string>(), "allow");
    BOOST_CHECK_EQUAL(names.values[0][1].get<std::string>(), "ready");
    BOOST_CHECK_EQUAL(names.values[0][2].get<std::string>(), "reconnect");
    BOOST_CHECK_EQUAL(names.values[0][3].get<std::string>(), "save");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(Lapi011Primitives)

BOOST_AUTO_TEST_CASE(LAPI_011_06_ConfigReflectsGlobalManager) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_cfg").dump());
    BOOST_REQUIRE(svc.success);
    CallResult cr =
        call(manager, svc.service_id, "config", nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    const nlohmann::json& cfg = cr.values[0];
    BOOST_CHECK_EQUAL(cfg["multi_device"].get<std::string>(), "single");
    BOOST_CHECK_EQUAL(cfg["message_queue_limit"].get<int>(), 8);
    BOOST_CHECK_EQUAL(cfg["anonymous"].get<bool>(), false);

    CallResult now = call(manager, svc.service_id, "now_ms_is_number",
                          nlohmann::json::array());
    BOOST_REQUIRE(now.success);
    BOOST_CHECK_EQUAL(now.values[0].get<bool>(), true);
}

BOOST_AUTO_TEST_CASE(LAPI_011_07_ManagerPrimitivesRoundTrip) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_mgr").dump());
    BOOST_REQUIRE(svc.success);

    // Register through the Lua primitive, then read back through C++.
    nlohmann::json ref = {{"uid", "u-prims"},
                          {"node_id", ""},
                          {"service_id", "player_u-prims"},
                          {"epoch", 0}};
    CallResult reg = call(manager, svc.service_id, "manager_register",
                          nlohmann::json::array({ref, "dev-1", "ready"}));
    BOOST_REQUIRE(reg.success);
    BOOST_CHECK_EQUAL(reg.values[0].get<bool>(), true);

    auto* pm = PlayerManager::global();
    BOOST_REQUIRE(pm != nullptr);
    auto info = pm->get("u-prims");
    BOOST_REQUIRE(info.has_value());
    BOOST_CHECK_EQUAL(info->ref.service_id, "player_u-prims");
    BOOST_CHECK_EQUAL(info->device_id, "dev-1");
    BOOST_CHECK(info->state == SessionState::kReady);

    // set_state through Lua flips what C++ sees.
    CallResult st = call(manager, svc.service_id, "manager_set_state",
                         nlohmann::json::array({"u-prims", "online"}));
    BOOST_REQUIRE(st.success);
    BOOST_CHECK_EQUAL(st.values[0].get<bool>(), true);
    BOOST_CHECK(pm->get("u-prims")->state == SessionState::kOnline);

    // get_devices + unregister round trip.
    CallResult devs = call(manager, svc.service_id, "manager_get_devices",
                           nlohmann::json::array({"u-prims"}));
    BOOST_REQUIRE(devs.success);
    BOOST_CHECK_EQUAL(devs.values[0].size(), 1u);

    CallResult unreg = call(manager, svc.service_id, "manager_unregister",
                            nlohmann::json::array({"u-prims"}));
    BOOST_REQUIRE(unreg.success);
    BOOST_CHECK_EQUAL(unreg.values[0].get<std::string>(), "player_u-prims");
    BOOST_CHECK(!pm->get("u-prims").has_value());

    // Unregistering again reports nil (not an error).
    CallResult missing = call(manager, svc.service_id, "manager_unregister",
                              nlohmann::json::array({"u-prims"}));
    BOOST_REQUIRE(missing.success);
    BOOST_CHECK(missing.values[0].is_null());
}

BOOST_AUTO_TEST_CASE(LAPI_011_08_ResolveErrorMatrix) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_res").dump());
    BOOST_REQUIRE(svc.success);

    auto* pm = PlayerManager::global();
    shield::player::PlayerRef seeded;
    seeded.uid = "u-res";
    seeded.service_id = "player_u-res";
    pm->register_session(seeded, "dev-1", SessionState::kReady, 1000);

    // invalid: uid missing
    CallResult invalid =
        call(manager, svc.service_id, "resolve",
             nlohmann::json::array({nlohmann::json::object()}));
    BOOST_REQUIRE(invalid.success);
    BOOST_CHECK(invalid.values[0].is_null());
    BOOST_CHECK_EQUAL(invalid.values[1].get<std::string>(),
                      "invalid_player_ref");

    // remote: non-local node_id (P0 resolves local sessions only)
    CallResult remote = call(manager, svc.service_id, "resolve",
                             nlohmann::json::array({nlohmann::json{
                                 {"uid", "u-res"}, {"node_id", "node-x"}}}));
    BOOST_REQUIRE(remote.success);
    BOOST_CHECK_EQUAL(remote.values[1].get<std::string>(),
                      "remote_resolve_unimplemented");

    // not found
    CallResult missing =
        call(manager, svc.service_id, "resolve",
             nlohmann::json::array({nlohmann::json{{"uid", "ghost"}}}));
    BOOST_REQUIRE(missing.success);
    BOOST_CHECK_EQUAL(missing.values[1].get<std::string>(), "player_not_found");

    // success: fields come back through write_session
    CallResult found =
        call(manager, svc.service_id, "resolve",
             nlohmann::json::array({nlohmann::json{{"uid", "u-res"}}}));
    BOOST_REQUIRE(found.success);
    BOOST_CHECK_EQUAL(found.values[0]["uid"].get<std::string>(), "u-res");
    BOOST_CHECK_EQUAL(found.values[0]["state"].get<std::string>(), "ready");
    BOOST_CHECK_EQUAL(found.values[0]["service_id"].get<std::string>(),
                      "player_u-res");
    BOOST_CHECK_EQUAL(found.values[0]["device_id"].get<std::string>(), "dev-1");

    pm->unregister("u-res");
}

BOOST_AUTO_TEST_CASE(LAPI_011_09_PlayerRefMaterializesFromMarker) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_ref").dump());
    BOOST_REQUIRE(svc.success);

    // The marker table arrives as the read-only PlayerRef userdata...
    nlohmann::json marker = {{"__shield_player_ref", true},
                             {"uid", "u-ref"},
                             {"node_id", "node-a"},
                             {"service_id", "player_u-ref"},
                             {"epoch", 5}};
    CallResult fields = call(manager, svc.service_id, "ref_fields",
                             nlohmann::json::array({marker}));
    BOOST_REQUIRE(fields.success);
    BOOST_CHECK_EQUAL(fields.values[0]["uid"].get<std::string>(), "u-ref");
    BOOST_CHECK_EQUAL(fields.values[0]["node_id"].get<std::string>(), "node-a");
    BOOST_CHECK_EQUAL(fields.values[0]["service_id"].get<std::string>(),
                      "player_u-ref");
    BOOST_CHECK_EQUAL(fields.values[0]["epoch"].get<std::string>(), "5");

    // ...and returning the userdata serializes back to the marker form.
    CallResult identity = call(manager, svc.service_id, "ref_identity",
                               nlohmann::json::array({marker}));
    BOOST_REQUIRE(identity.success);
    BOOST_CHECK_EQUAL(identity.values[0]["__shield_player_ref"].get<bool>(),
                      true);
    BOOST_CHECK_EQUAL(identity.values[0]["uid"].get<std::string>(), "u-ref");
    BOOST_CHECK_EQUAL(identity.values[0]["epoch"].get<uint64_t>(), 5u);
}

BOOST_AUTO_TEST_CASE(LAPI_011_10_GuardPassesWithoutSession) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_guard").dump());
    BOOST_REQUIRE(svc.success);

    // Before setup the guard does not exist at all.
    CallResult pre =
        call(manager, svc.service_id, "do_guard",
             nlohmann::json::array({nullptr, "some_route", nullptr}));
    BOOST_REQUIRE(pre.success);
    BOOST_CHECK_EQUAL(pre.values[0]["installed"].get<bool>(), false);

    CallResult setup = call(manager, svc.service_id, "do_setup",
                            nlohmann::json::array({"full", nullptr, nullptr}));
    BOOST_REQUIRE(setup.success);

    // After setup, a VM without a session (auth-entry / pre-bind) passes
    // through untouched: only bound players are guarded.
    CallResult post =
        call(manager, svc.service_id, "do_guard",
             nlohmann::json::array({nullptr, "some_route", nullptr}));
    BOOST_REQUIRE(post.success);
    BOOST_CHECK_EQUAL(post.values[0]["installed"].get<bool>(), true);
    BOOST_CHECK_EQUAL(post.values[0]["allowed"].get<bool>(), true);
}

BOOST_AUTO_TEST_SUITE_END()

// Full lifecycle through the gateway: bind -> Bound -> login/ready ->
// guard -> push -> disconnect -> offline queue -> restore + flush.
BOOST_AUTO_TEST_SUITE(Lapi011EndToEnd)

// Drives authenticate on @p auth for @p uid over @p session_id and waits
// until the spawned instance reached the ready state. Returns the
// do_authenticate result.
CallResult authenticate_and_wait_ready(GatewayWorld& world,
                                       const std::string& auth_service,
                                       const std::string& uid,
                                       uint64_t session_id,
                                       MockSession& session) {
    CallResult result =
        call(world.manager, auth_service, "do_authenticate",
             nlohmann::json::array(
                 {client_marker("gw", session_id, session.epoch(), ""),
                  {{"player_id", uid}, {"device_id", "dev-1"}}}));
    BOOST_REQUIRE(result.success);
    BOOST_CHECK_EQUAL(result.values[0]["ok"].get<bool>(), true);
    auto* pm = PlayerManager::global();
    BOOST_CHECK(wait_until(
        [&]() {
            auto info = pm->get(uid);
            return info.has_value() && info->state == SessionState::kReady;
        },
        std::chrono::seconds(5)));
    return result;
}

BOOST_AUTO_TEST_CASE(LAPI_011_15_GuardEnforcesReadyAndBusinessBlock) {
    GatewayWorld world;
    auto auth =
        world.manager.spawn(PLAYER_SCRIPT, service_opts("auth_gate").dump());
    BOOST_REQUIRE(auth.success);
    CallResult setup =
        call(world.manager, auth.service_id, "do_setup",
             nlohmann::json::array({"full", PLAYER_SCRIPT, test_routes()}));
    BOOST_REQUIRE(setup.success);

    auto session = connect_session(*world.registry, 205);
    authenticate_and_wait_ready(world, auth.service_id, "u-gate", 205,
                                *session);

    // The guard on the INSTANCE VM (where the session lives): the business
    // client_message hook may reject with its own code.
    CallResult blocked =
        call(world.manager, "player_u-gate", "do_guard",
             nlohmann::json::array({nullptr, "route_x", {{"block", true}}}));
    BOOST_REQUIRE(blocked.success);
    BOOST_CHECK_EQUAL(blocked.values[0]["allowed"].get<bool>(), false);
    BOOST_CHECK_EQUAL(blocked.values[0]["code"].get<std::string>(), "blocked");

    // Ready-state truth lives in the PlayerManager: flip the manager state
    // to online (login completed but the client has not announced ready)
    // and the guard rejects with the stable code.
    CallResult flip = call(world.manager, "player_u-gate", "manager_set_state",
                           nlohmann::json::array({"u-gate", "online"}));
    BOOST_REQUIRE(flip.success);

    CallResult not_ready =
        call(world.manager, "player_u-gate", "do_guard",
             nlohmann::json::array({nullptr, "route_x", nullptr}));
    BOOST_REQUIRE(not_ready.success);
    BOOST_CHECK_EQUAL(not_ready.values[0]["allowed"].get<bool>(), false);
    BOOST_CHECK_EQUAL(not_ready.values[0]["code"].get<std::string>(),
                      "not_ready");

    // Back to ready: messages flow again.
    CallResult restore =
        call(world.manager, "player_u-gate", "manager_set_state",
             nlohmann::json::array({"u-gate", "ready"}));
    BOOST_REQUIRE(restore.success);
    CallResult ok = call(world.manager, "player_u-gate", "do_guard",
                         nlohmann::json::array({nullptr, "route_x", nullptr}));
    BOOST_REQUIRE(ok.success);
    BOOST_CHECK_EQUAL(ok.values[0]["allowed"].get<bool>(), true);

    // Exactly one rejection of each kind was counted.
    CallResult stats =
        call(world.manager, "player_u-gate", "stats", nlohmann::json::array());
    BOOST_REQUIRE(stats.success);
    BOOST_CHECK_EQUAL(stats.values[0]["rejected_not_ready"].get<int>(), 1);
    BOOST_CHECK_EQUAL(stats.values[0]["rejected_by_guard"].get<int>(), 1);

    PlayerManager::global()->unregister("u-gate");
}

BOOST_AUTO_TEST_CASE(LAPI_011_16_PushOnlineQueueAndRestoreFlush) {
    GatewayWorld world;
    auto auth =
        world.manager.spawn(PLAYER_SCRIPT, service_opts("auth_push").dump());
    BOOST_REQUIRE(auth.success);
    CallResult setup =
        call(world.manager, auth.service_id, "do_setup",
             nlohmann::json::array({"full", PLAYER_SCRIPT, test_routes()}));
    BOOST_REQUIRE(setup.success);

    auto session = connect_session(*world.registry, 206);
    authenticate_and_wait_ready(world, auth.service_id, "u-push", 206,
                                *session);

    // A push for a uid this service does not host is a stable error, not a
    // crash or a silent send.
    CallResult missing = call(
        world.manager, "player_u-push", "do_push",
        nlohmann::json::array({"u-nobody", "login_result", {{"hello", 1}}}));
    BOOST_REQUIRE(missing.success);
    BOOST_CHECK_EQUAL(missing.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(missing.values[1]["code"].get<std::string>(),
                      "player_not_found");

    // Online push: straight through the s2c helper onto the wire.
    CallResult online =
        call(world.manager, "player_u-push", "do_push",
             nlohmann::json::array({"u-push", "login_result", {{"hello", 1}}}));
    BOOST_REQUIRE(online.success);
    BOOST_CHECK_EQUAL(online.values[0].get<bool>(), true);
    BOOST_CHECK(
        wait_until([&]() { return session->sent_messages().size() == 1u; },
                   std::chrono::seconds(5)));
    BOOST_CHECK_EQUAL(session->sent_messages()[0].route_id, 1001u);

    // Disconnect the session: the instance parks in the reconnect window.
    ClientControlMessage gone;
    gone.kind = ClientControlMessage::Kind::Disconnected;
    gone.context = ClientContextData{"gw", 206, 1, "u-push", "json"};
    gone.reason = "link_lost";
    caf::anon_send(world.manager.service_actor("player_u-push"), gone);
    BOOST_CHECK(wait_until(
        [&]() {
            for (const auto& entry : calls(world.manager, "player_u-push")) {
                if (entry["name"] == "disconnect") return true;
            }
            return false;
        },
        std::chrono::seconds(5)));

    // Offline pushes queue instead of sending (the wire stays flat).
    bool queue_full = false;
    for (int i = 0; i < 9; ++i) {
        CallResult queued = call(
            world.manager, "player_u-push", "do_push",
            nlohmann::json::array({"u-push", "login_result", {{"queued", i}}}));
        BOOST_REQUIRE(queued.success);
        if (!queued.values[0].get<bool>()) {
            // The 9th push (the module fixture's queue limit is 8) reports
            // the stable overflow code.
            BOOST_REQUIRE_EQUAL(i, 8);
            BOOST_CHECK_EQUAL(queued.values[1]["code"].get<std::string>(),
                              "offline_queue_full");
            queue_full = true;
        }
    }
    BOOST_CHECK(queue_full);
    BOOST_CHECK_EQUAL(session->sent_messages().size(), 1u);

    // Restore inside the reconnect window: the live instance receives the
    // fresh ClientRef, flushes its queue in order, and returns to ready.
    CallResult result =
        call(world.manager, auth.service_id, "do_authenticate",
             nlohmann::json::array(
                 {client_marker("gw", 206, session->epoch(), "u-push"),
                  {{"player_id", "u-push"}, {"device_id", "dev-1"}}}));
    BOOST_REQUIRE(result.success);
    BOOST_CHECK_EQUAL(result.values[0]["ok"].get<bool>(), true);
    BOOST_CHECK_EQUAL(result.values[0]["epoch"].get<uint32_t>(), 2u);

    // 1 online + 8 flushed = 9 on the wire; the dropped overflow stays
    // dropped.
    BOOST_CHECK(
        wait_until([&]() { return session->sent_messages().size() == 9u; },
                   std::chrono::seconds(5)));

    bool saw_reconnect = false;
    for (const auto& entry : calls(world.manager, "player_u-push")) {
        if (entry["name"] == "reconnect_custom") saw_reconnect = true;
    }
    BOOST_CHECK(saw_reconnect);

    CallResult stats =
        call(world.manager, "player_u-push", "stats", nlohmann::json::array());
    BOOST_REQUIRE(stats.success);
    BOOST_CHECK_EQUAL(stats.values[0]["offline_dropped"].get<int>(), 1);

    auto* pm = PlayerManager::global();
    BOOST_CHECK(pm->get("u-push")->state == SessionState::kReady);
    pm->unregister("u-push");
}

BOOST_AUTO_TEST_SUITE_END()

#else  // SHIELD_ENABLE_PLAYER

// The player-enabled build initializes the CAF global meta objects through
// its PlayerGlobalFixture; the stub build has no fixture of its own and the
// actor_system below aborts without them.
struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(Lapi011Stub)

BOOST_AUTO_TEST_CASE(LAPI_011_17_StubReportsModuleUnavailable) {
    caf::actor_system_config caf_cfg;
    caf::actor_system system(caf_cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto svc = manager.spawn(PLAYER_SCRIPT, service_opts("auth_stub").dump());
    BOOST_REQUIRE(svc.success);

    // setup returns nil + the stable module_unavailable error table.
    CallResult setup = call(manager, svc.service_id, "do_setup",
                            nlohmann::json::array({"full", nullptr, nullptr}));
    BOOST_REQUIRE(setup.success);
    BOOST_CHECK_EQUAL(setup.values[0]["ok"].get<bool>(), false);
    BOOST_CHECK_EQUAL(setup.values[0]["code"].get<std::string>(),
                      "module_unavailable");

    // The other entries degrade the same way instead of erroring.
    CallResult resolved =
        call(manager, svc.service_id, "resolve",
             nlohmann::json::array({nlohmann::json{{"uid", "u"}}}));
    BOOST_REQUIRE(resolved.success);
    BOOST_CHECK_EQUAL(resolved.values[1].get<std::string>(),
                      "module_unavailable");

    CallResult stats =
        call(manager, svc.service_id, "stats", nlohmann::json::array());
    BOOST_REQUIRE(stats.success);
    BOOST_CHECK(stats.values[0].is_null());
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // SHIELD_ENABLE_PLAYER
