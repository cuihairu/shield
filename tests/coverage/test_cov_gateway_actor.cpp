// Coverage tests for src/lua/gateway_actor.cpp: the registry, the egress /
// bind / close validation gates, and the actor wiring. The free entry points
// run on the test thread; only the spawn_gateway_actor smoke case goes
// through CAF.
#define BOOST_TEST_MODULE CovGatewayActor
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
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
        ++close_count_;
    }
    bool is_alive() const override { return alive_; }
    std::string error_code() const override {
        return alive_ ? "" : "session_closed";
    }
    bool has_protocol_pipeline() const override { return false; }
    std::string_view protocol_codec_name() const override { return {}; }
    bool send_message(const shield::transport::DecodedBody& message,
                      std::string* error) override {
        if (!allow_send_) {
            if (error) *error = "send queue full";
            return false;
        }
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

    // Test-side controls and read helpers (not part of the interface).
    void set_allow_send(bool allow) { allow_send_ = allow; }
    const std::vector<shield::transport::DecodedBody>& sent_messages() const {
        return sent_messages_;
    }
    const std::string& close_reason() const { return close_reason_; }
    int close_count() const { return close_count_; }
    uint32_t epoch() const { return binding_.epoch; }
    std::string target_service() const { return binding_.target_service; }

private:
    shield::net::SessionId id_;
    shield::net::RemoteAddress remote_;
    bool alive_ = true;
    bool allow_send_ = true;
    std::string close_reason_;
    int close_count_ = 0;
    std::vector<shield::transport::DecodedBody> sent_messages_;
    std::unordered_map<std::string, std::string> user_data_;
    shield::net::SessionBinding binding_;
};

std::shared_ptr<MockSession> make_session(shield::net::SessionId id,
                                          shield::net::SessionBinding binding) {
    auto session = std::make_shared<MockSession>(
        id, shield::net::RemoteAddress{"127.0.0.1", 40000});
    session->reset_binding(std::move(binding));
    return session;
}

shield::net::SessionBinding auth_binding(std::string gateway = "gw") {
    shield::net::SessionBinding binding;
    binding.target_service = "auth";
    binding.gateway_name = std::move(gateway);
    binding.protocol_profile_id = "json";
    binding.epoch = 0;
    return binding;
}

ClientContextData context_of(const std::shared_ptr<MockSession>& session) {
    const shield::net::SessionBinding binding = session->binding();
    return ClientContextData{"gw", session->id(), binding.epoch,
                             binding.player_id, binding.protocol_profile_id};
}

shield::transport::RpcDescriptorTable s2c_table() {
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

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(GatewayActorSuite)

// -- GatewaySessionRegistry
// ----------------------------------------------------

BOOST_AUTO_TEST_CASE(RegistryAddFindRemoveAndSize) {
    auto registry = std::make_shared<GatewaySessionRegistry>();
    BOOST_CHECK_EQUAL(registry->size(), 0u);
    BOOST_CHECK(registry->find(1) == nullptr);

    auto first = make_session(1, auth_binding());
    auto second = make_session(2, auth_binding());
    registry->add(first, first->binding());
    registry->add(second, second->binding());
    BOOST_CHECK_EQUAL(registry->size(), 2u);
    BOOST_CHECK(registry->find(1) == first);
    BOOST_CHECK(registry->find(2) == second);
    BOOST_CHECK(registry->find(3) == nullptr);

    registry->remove(1);
    BOOST_CHECK_EQUAL(registry->size(), 1u);
    // Idempotent remove: disconnect and close may race.
    registry->remove(1);
    registry->remove(999);
    BOOST_CHECK_EQUAL(registry->size(), 1u);
    BOOST_CHECK(registry->find(1) == nullptr);
    BOOST_CHECK(registry->find(2) == second);
}

BOOST_AUTO_TEST_CASE(RegistryReapsExpiredWeakHandle) {
    auto registry = std::make_shared<GatewaySessionRegistry>();
    {
        auto session = make_session(7, auth_binding());
        registry->add(session, session->binding());
        BOOST_CHECK_EQUAL(registry->size(), 1u);
        BOOST_CHECK(registry->find(7) == session);
    }  // session dies without a disconnect callback

    // Lazy reap: find() drops the expired entry instead of returning a dead
    // handle, so egress can never resurrect a gone session.
    BOOST_CHECK(registry->find(7) == nullptr);
    BOOST_CHECK_EQUAL(registry->size(), 0u);
}

// -- handle_client_egress
// --------------------------------------------------------

BOOST_AUTO_TEST_CASE(EgressGateOrderAndAcceptance) {
    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), nullptr};

    auto session = make_session(10, auth_binding());
    registry->add(session, session->binding());

    const ClientContextData ctx = context_of(session);

    // 1. Unknown session.
    ClientEgress egress;
    egress.context = ctx;
    egress.context.session_id = 404;
    egress.route_id = 1001;
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_unknown_session.load(), 1u);

    // 2. Stale epoch.
    egress.context = ctx;
    egress.context.session_epoch = ctx.session_epoch + 1;
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_stale_epoch.load(), 1u);

    // 3. Owner mismatch.
    egress.context = ctx;
    egress.context.player_id = "someone-else";
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_owner_mismatch.load(), 1u);

    // 4. Route not found.
    egress.context = ctx;
    egress.route_id = 9999;
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_route_not_found.load(), 1u);

    // 5. Direction rejected: a c2s route can never carry egress.
    shield::transport::RpcDescriptor c2s;
    c2s.route_id = 1002;
    c2s.name = "login";
    c2s.direction = shield::transport::RouteDirection::ClientToServer;
    shield::transport::RpcDescriptorTable with_c2s = s2c_table();
    with_c2s.add(c2s);
    deps.descriptors = std::move(with_c2s);
    egress.route_id = 1002;
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_direction_rejected.load(), 1u);

    // 6. Accepted, bytes form: route name rides along for diagnostics.
    deps.descriptors = s2c_table();
    egress.route_id = 1001;
    egress.body_bytes = {'b', 'y', 't', 'e'};
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_accepted.load(), 1u);
    BOOST_REQUIRE_EQUAL(session->sent_messages().size(), 1u);
    BOOST_CHECK(!session->sent_messages()[0].has_message());
    BOOST_CHECK((session->sent_messages()[0].bytes ==
                 std::vector<uint8_t>{'b', 'y', 't', 'e'}));
    BOOST_CHECK_EQUAL(session->sent_messages()[0].route_name, "login_result");

    // 7. Accepted, structured message form.
    egress.message = nlohmann::json{{"level", 2}};
    egress.body_bytes.clear();
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_accepted.load(), 2u);
    BOOST_REQUIRE_EQUAL(session->sent_messages().size(), 2u);
    BOOST_REQUIRE(session->sent_messages()[1].has_message());
    BOOST_CHECK_EQUAL(
        (*session->sent_messages()[1].message)["level"].get<int>(), 2);

    // 8. Bidirectional routes carry egress too.
    shield::transport::RpcDescriptor bidi;
    bidi.route_id = 1003;
    bidi.name = "chat";
    bidi.direction = shield::transport::RouteDirection::Bidirectional;
    shield::transport::RpcDescriptorTable with_bidi = s2c_table();
    with_bidi.add(bidi);
    deps.descriptors = std::move(with_bidi);
    egress.route_id = 1003;
    egress.message.reset();
    egress.body_bytes = {'x'};
    handle_client_egress(deps, egress);
    BOOST_CHECK_EQUAL(stats->egress_accepted.load(), 3u);
    BOOST_CHECK_EQUAL(stats->egress_send_failed.load(), 0u);
}

BOOST_AUTO_TEST_CASE(EgressSendFailureCountsNotRetries) {
    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), nullptr};

    auto session = make_session(11, auth_binding());
    registry->add(session, session->binding());
    session->set_allow_send(false);  // backpressure / closed socket

    ClientEgress egress;
    egress.context = context_of(session);
    egress.route_id = 1001;
    egress.body_bytes = {'b'};
    handle_client_egress(deps, egress);

    BOOST_CHECK_EQUAL(stats->egress_send_failed.load(), 1u);
    BOOST_CHECK_EQUAL(stats->egress_accepted.load(), 0u);
    BOOST_CHECK(session->sent_messages().empty());
}

// -- handle_client_bind -------------------------------------------------------

BOOST_AUTO_TEST_CASE(BindSuccessCompletesCallWithFreshMarker) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), &manager};

    auto session = make_session(20, auth_binding());
    registry->add(session, session->binding());

    std::vector<std::tuple<uint64_t, bool, nlohmann::json>> completions;
    manager.set_proxied_call_hook(
        [&](uint64_t session, bool ok, const nlohmann::json& values) {
            completions.emplace_back(session, ok, values);
        });

    ClientBindRequest request;
    request.call_session = manager.begin_proxied_call(5000);
    request.sender_service = "auth";
    request.context = context_of(session);
    request.player_id = "player-20";
    request.target_service = "player";
    handle_client_bind(deps, request);

    BOOST_CHECK_EQUAL(stats->binds_ok.load(), 1u);
    BOOST_CHECK_EQUAL(stats->binds_epoch_expired.load(), 0u);
    BOOST_CHECK_EQUAL(session->target_service(), "player");
    BOOST_CHECK_EQUAL(session->binding().player_id, "player-20");
    BOOST_CHECK_EQUAL(session->epoch(), 1u);

    // The waiter gets exactly one success carrying the fresh reference
    // marker: new epoch, new player, this gateway as the egress route.
    BOOST_REQUIRE_EQUAL(completions.size(), 1u);
    BOOST_CHECK_EQUAL(std::get<0>(completions[0]), request.call_session);
    BOOST_CHECK(std::get<1>(completions[0]));
    const nlohmann::json& values = std::get<2>(completions[0]);
    BOOST_REQUIRE(values.is_array());
    BOOST_REQUIRE_EQUAL(values.size(), 1u);
    BOOST_CHECK_EQUAL(values[0]["session_epoch"].get<uint32_t>(), 1u);
    BOOST_CHECK_EQUAL(values[0]["player_id"].get<std::string>(), "player-20");
    BOOST_CHECK_EQUAL(values[0]["gateway_address"].get<std::string>(), "gw");

    // The registry snapshot was refreshed alongside the CAS.
    BOOST_CHECK(registry->find(20) == session);
}

BOOST_AUTO_TEST_CASE(BindWithoutWaiterAppliesSilently) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), &manager};

    auto session = make_session(21, auth_binding());
    registry->add(session, session->binding());

    bool hook_called = false;
    manager.set_proxied_call_hook(
        [&](uint64_t, bool, const nlohmann::json&) { hook_called = true; });

    ClientBindRequest request;
    request.call_session = 0;  // fire-and-forget: no completion, no hook
    request.context = context_of(session);
    request.player_id = "player-21";
    request.target_service = "player";
    handle_client_bind(deps, request);

    BOOST_CHECK_EQUAL(stats->binds_ok.load(), 1u);
    BOOST_CHECK_EQUAL(session->epoch(), 1u);
    BOOST_CHECK(!hook_called);
}

BOOST_AUTO_TEST_CASE(BindFailuresRouteEpochExpired) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), &manager};

    auto session = make_session(22, auth_binding());
    registry->add(session, session->binding());

    std::vector<std::tuple<uint64_t, bool, nlohmann::json>> completions;
    manager.set_proxied_call_hook(
        [&](uint64_t session, bool ok, const nlohmann::json& values) {
            completions.emplace_back(session, ok, values);
        });

    // 1. Unknown session (never connected or already reaped).
    ClientBindRequest request;
    request.call_session = manager.begin_proxied_call(5000);
    request.context = context_of(session);
    request.context.session_id = 404;
    request.player_id = "player-22";
    request.target_service = "player";
    handle_client_bind(deps, request);
    BOOST_CHECK_EQUAL(stats->binds_epoch_expired.load(), 1u);
    BOOST_REQUIRE_EQUAL(completions.size(), 1u);
    BOOST_CHECK(!std::get<1>(completions[0]));
    BOOST_CHECK_EQUAL(std::get<2>(completions[0])[0]["code"].get<std::string>(),
                      "client_rpc.epoch_expired");

    // 2. Stale epoch: a fire-and-forget bind bumps the live epoch to 1; the
    // old reference (epoch 0) must then fail the CAS.
    request.call_session = 0;
    request.context = context_of(session);  // epoch 0, live epoch 0: applies
    request.context.session_id = 22;
    request.player_id = "player-22b";
    request.target_service = "player";
    handle_client_bind(deps, request);
    BOOST_CHECK_EQUAL(stats->binds_ok.load(), 1u);
    BOOST_CHECK_EQUAL(session->epoch(), 1u);

    completions.clear();
    request.call_session = manager.begin_proxied_call(5000);
    request.context = context_of(session);  // reads live epoch 1...
    request.context.session_epoch = 0;      // ...but presents the stale one
    handle_client_bind(deps, request);
    BOOST_CHECK_EQUAL(stats->binds_epoch_expired.load(), 2u);
    BOOST_CHECK_EQUAL(stats->binds_ok.load(), 1u);
    BOOST_REQUIRE_EQUAL(completions.size(), 1u);
    BOOST_CHECK(!std::get<1>(completions[0]));
    BOOST_CHECK_EQUAL(session->epoch(), 1u);  // untouched by the failed CAS

    // 3. Fire-and-forget failure: counted, no completion, no crash.
    completions.clear();
    request.call_session = 0;
    request.context.session_id = 404;
    handle_client_bind(deps, request);
    BOOST_CHECK_EQUAL(stats->binds_epoch_expired.load(), 3u);
    BOOST_CHECK(completions.empty());
}

// -- handle_client_close ------------------------------------------------------

BOOST_AUTO_TEST_CASE(CloseInvalidatesRemovesAndKicks) {
    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), nullptr};

    auto session = make_session(30, auth_binding());
    registry->add(session, session->binding());
    // Simulate a post-login binding so invalidation has something to wipe.
    session->apply_binding("player", "player-30", shield::net::kAnyEpoch,
                           nullptr);

    ClientCloseRequest request;
    request.context = context_of(session);
    request.reason = "ban";
    handle_client_close(deps, request);

    // Binding invalidated (epoch bumped, identity wiped) before the socket
    // actually closes: outstanding stale references must fail immediately.
    BOOST_CHECK_EQUAL(session->binding().target_service, "");
    BOOST_CHECK_EQUAL(session->binding().player_id, "");
    BOOST_CHECK_GT(session->epoch(), 1u);
    BOOST_CHECK(!session->is_alive());
    BOOST_CHECK_EQUAL(session->close_reason(), "ban");
    BOOST_CHECK_EQUAL(session->close_count(), 1);
    BOOST_CHECK_EQUAL(stats->close_requests.load(), 1u);
    BOOST_CHECK_EQUAL(registry->size(), 0u);
    BOOST_CHECK(registry->find(30) == nullptr);

    // Unknown session: counted, warned, no crash.
    ClientCloseRequest unknown;
    unknown.context.session_id = 404;
    handle_client_close(deps, unknown);
    BOOST_CHECK_EQUAL(stats->close_requests.load(), 2u);
}

BOOST_AUTO_TEST_CASE(CloseWithEmptyReasonUsesKickedDefault) {
    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), nullptr};

    auto session = make_session(31, auth_binding());
    registry->add(session, session->binding());

    ClientCloseRequest request;
    request.context = context_of(session);
    handle_client_close(deps, request);

    BOOST_CHECK_EQUAL(session->close_reason(),
                      std::string(shield::net::CloseReason::KICKED));
}

// With a live target service actor, a bind delivers a Bound control message
// and a close delivers Unbound to that target (production notify paths).
BOOST_AUTO_TEST_CASE(BindAndCloseNotifyTargetActor) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    const char* target_script = R"lua(
local M = {}
local log = {}
function M.on_init(args) end
function M.on_client_bound(ctx, client)
  table.insert(log, {"bound", client:session_id()}) return true
end
function M.on_client_unbound(ctx, client, reason)
  table.insert(log, {"unbound", client:session_id(), reason}) return true
end
function M.get_log(ctx) return log end
return M
)lua";
    const std::string target_path = "/tmp/opencode/cov_gw_target.lua";
    {
        std::ofstream out(target_path, std::ios::trunc);
        out << target_script;
    }
    auto svc = manager.spawn(target_path, R"({"name": "cov_gw_target"})");
    BOOST_REQUIRE(svc.success);

    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw", registry, stats, s2c_table(), &manager};

    auto session = make_session(32, auth_binding());
    registry->add(session, session->binding());

    // Bind to the live target: the notify branch sends Bound to its actor.
    ClientBindRequest request;
    request.context = context_of(session);
    request.player_id = "player-32";
    request.target_service = svc.service_id;
    handle_client_bind(deps, request);
    BOOST_CHECK_EQUAL(stats->binds_ok.load(), 1u);

    // Close: the Unbound notify branch fires before the binding is cleared.
    ClientCloseRequest close_request;
    close_request.context = context_of(session);
    close_request.reason = "gw_kick";
    handle_client_close(deps, close_request);
    BOOST_CHECK_EQUAL(session->close_count(), 1);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult log = manager.call(svc.service_id, "get_log",
                                          nlohmann::json::array());
            if (!log.success || !log.values.is_array() ||
                log.values.size() != 1u || !log.values[0].is_array() ||
                log.values[0].size() != 2u) {
                return false;
            }
            const auto& entries = log.values[0];
            return entries[0].is_array() && entries[0][0] == "bound" &&
                   entries[0][1].get<uint64_t>() == 32u &&
                   entries[1].is_array() && entries[1][0] == "unbound" &&
                   entries[1][1].get<uint64_t>() == 32u &&
                   entries[1][2] == "gw_kick";
        },
        std::chrono::seconds(3)));
}

// -- spawn_gateway_actor ------------------------------------------------------

BOOST_AUTO_TEST_CASE(SpawnedActorHandlesAllThreeMessageTypes) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    auto registry = std::make_shared<GatewaySessionRegistry>();
    auto stats = std::make_shared<GatewayStats>();
    GatewayDeps deps{"gw_actor", registry, stats, s2c_table(), &manager};
    caf::actor gateway = spawn_gateway_actor(system, std::move(deps));

    auto session = make_session(40, auth_binding("gw_actor"));
    registry->add(session, session->binding());

    // Egress through the actor mailbox.
    ClientEgress egress;
    egress.context = context_of(session);
    egress.route_id = 1001;
    egress.body_bytes = {'a'};
    caf::anon_send(gateway, std::move(egress));

    // Bind through the actor mailbox, with a proxied waiter.
    std::vector<std::pair<uint64_t, bool>> completions;
    manager.set_proxied_call_hook(
        [&](uint64_t session, bool ok, const nlohmann::json&) {
            completions.emplace_back(session, ok);
        });
    ClientBindRequest request;
    request.call_session = manager.begin_proxied_call(5000);
    request.context = context_of(session);
    request.player_id = "player-40";
    request.target_service = "player";
    caf::anon_send(gateway, std::move(request));

    BOOST_CHECK(wait_until(
        [&]() {
            return stats->egress_accepted.load() == 1u &&
                   stats->binds_ok.load() == 1u && completions.size() == 1u;
        },
        std::chrono::seconds(2)));
    BOOST_CHECK_EQUAL(session->epoch(), 1u);
    BOOST_CHECK_EQUAL(session->target_service(), "player");

    // Close through the actor mailbox.
    ClientCloseRequest close_request;
    close_request.context = context_of(session);
    close_request.context.player_id = "player-40";
    close_request.context.session_epoch = session->epoch();
    close_request.reason = "kick";
    caf::anon_send(gateway, std::move(close_request));

    BOOST_CHECK(wait_until(
        [&]() {
            return stats->close_requests.load() == 1u && !session->is_alive() &&
                   registry->size() == 0u;
        },
        std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace
