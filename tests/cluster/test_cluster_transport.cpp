// ClusterTransport integration tests (M2): two real caf::actor_systems in
// one process, talking over CAF's BASP on the loopback. Covers the full
// handshake (identity adoption on the dialer side), heartbeat keep-alive,
// peer-down handling and reconnect-after-restart with epoch change.
//
// The cluster tests only run on Linux CI; the port helper uses POSIX
// sockets on purpose (no Boost.Asio dependency in the test binary).
#define BOOST_TEST_MODULE ClusterTransportTests
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/io/middleman.hpp>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>

#include "shield/caf_initializer.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using shield::cluster::ClusterConfig;
using shield::cluster::ClusterManager;
using shield::cluster::ClusterTransport;
using shield::cluster::node_state_name;
using shield::cluster::NodeState;
using shield::lua::LuaRuntime;
using shield::lua::LuaServiceManager;

namespace {

// Mirror transport/manager logs to stderr so handshake failures are
// diagnosable from the ctest output.
class StderrLogSink : public shield::log::LogSink {
public:
    void write(const shield::log::LogRecord& record) override {
        static std::mutex mutex;
        const char* level = "info";
        switch (record.level) {
            case shield::log::Level::Debug:
                level = "debug";
                break;
            case shield::log::Level::Info:
                level = "info";
                break;
            case shield::log::Level::Warning:
                level = "warn";
                break;
            case shield::log::Level::Error:
                level = "error";
                break;
            case shield::log::Level::Fatal:
                level = "fatal";
                break;
        }
        std::lock_guard lock(mutex);
        std::cerr << "[" << level << "] " << record.logger_name << ": "
                  << record.message << "\n";
    }
    void flush() override { std::cerr.flush(); }
};

void enable_test_logging() {
    static bool done = false;
    if (done) return;
    done = true;
    shield::log::Logger::add_sink(std::make_unique<StderrLogSink>());
    shield::log::Logger::set_global_level(shield::log::Level::Debug);
}

// Ask the kernel for a free loopback port via the usual bind(0) probe.
// The probe closes its socket, so the port can be lost to any other
// ephemeral-port consumer on the machine before the CAF listener re-binds
// it; make_node() reports that as a failed start and make_node_pair()
// below retries the whole pair on fresh ports.
uint16_t free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    BOOST_REQUIRE_EQUAL(
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE_EQUAL(
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    ::close(fd);
    return ntohs(addr.sin_port);
}

// One cluster node: manager + CAF system + published transport. Held through
// a unique_ptr so nothing is ever moved. Both peers' ports must be known
// before construction, so tests pick them up front.
struct Node {
    ClusterConfig config;
    std::unique_ptr<ClusterManager> manager;
    caf::actor_system_config caf_config;
    std::unique_ptr<caf::actor_system> system;
    std::unique_ptr<ClusterTransport> transport;
    // M4 data plane (attached on demand by attach_data_plane below).
    std::shared_ptr<LuaRuntime> runtime;
    std::shared_ptr<LuaServiceManager> services;
};

std::unique_ptr<Node> make_node(
    const std::string& node_id, uint16_t listen, uint16_t peer_port,
    const std::function<void(Node&)>& configure = {}) {
    // CAF requires core + io middleman meta objects, and our wire types,
    // registered before any actor_system exists (same trio as
    // initialize_caf_types() plus the cluster block).
    caf::core::init_global_meta_objects();
    caf::io::middleman::init_global_meta_objects();
    shield::cluster::init_cluster_caf_types();
    auto node = std::make_unique<Node>();
    node->config.enabled = true;
    node->config.node_id = node_id;
    node->config.listen_address = "127.0.0.1:" + std::to_string(listen);
    node->config.peers = {"127.0.0.1:" + std::to_string(peer_port)};
    // Short heartbeat cadence; generous suspect window so a single lost
    // tick cannot flake the keep-alive assertions.
    node->config.heartbeat_interval_ms = 50;
    node->config.suspect_timeout_ms = 800;
    node->config.offline_timeout_ms = 4000;
    node->manager = std::make_unique<ClusterManager>(node->config);
    node->manager->start();
    node->caf_config.load<caf::io::middleman>();
    node->system = std::make_unique<caf::actor_system>(node->caf_config);
    // Hook for per-test state (e.g. local routes) that must exist before
    // the transport — and therefore any handshake traffic — starts.
    if (configure) configure(*node);
    node->transport = std::make_unique<ClusterTransport>(
        *node->system, *node->manager, node->config);
    uint16_t bound = 0;
    std::string error;
    if (!node->transport->start(&bound, error)) {
        // Most likely cause: the port handed out by free_port() was taken
        // between its close() and this bind. Return null so the caller can
        // retry on fresh ports instead of failing the test case.
        BOOST_TEST_MESSAGE(
            "make_node(" << node_id << "): transport start failed: " << error);
        return nullptr;
    }
    BOOST_CHECK_EQUAL(bound, listen);
    return node;
}

// Bring up the standard two-node cluster used by the IT cases below.
//
// Both nodes learn each other's port up front, so a listen port lost to
// the free_port() race (see above) cannot be repaired in place: the peer
// would keep dialing the stale address. Tear the pair down and retry with
// fresh ports instead; `port_a`/`port_b` are updated to the ports that
// were actually bound. `a_first` keeps each test's original construction
// order (which test asserts timing semantics about who dials first).
std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>> make_node_pair(
    uint16_t& port_a, uint16_t& port_b, bool a_first = true,
    const std::function<void(Node&)>& configure_a = {},
    const std::function<void(Node&)>& configure_b = {}) {
    for (int attempt = 1; attempt <= 5; ++attempt) {
        port_a = free_port();
        port_b = free_port();
        std::unique_ptr<Node> a, b;
        if (a_first) {
            a = make_node("node-a", port_a, port_b, configure_a);
            if (!a) continue;
            b = make_node("node-b", port_b, port_a, configure_b);
        } else {
            b = make_node("node-b", port_b, port_a, configure_b);
            if (!b) continue;
            a = make_node("node-a", port_a, port_b, configure_a);
        }
        if (a && b) return {std::move(a), std::move(b)};
        // The built node (if any) is destroyed by scope exit: both dtors
        // stop cleanly, including a transport that never started.
        BOOST_TEST_MESSAGE("node pair attempt " << attempt
                                                << " lost a port, retrying");
    }
    BOOST_FAIL("could not bring up a two-node cluster on free ports");
    return {};
}

// Poll until node `id` on `mgr` reaches `expected` or the budget runs out.
bool wait_for_state(ClusterManager& mgr, const std::string& id,
                    NodeState expected, std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto* node = mgr.find_node(id);
        if (node && node->state == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::string state_of(ClusterManager& mgr, const std::string& id) {
    const auto* node = mgr.find_node(id);
    return node ? node_state_name(node->state) : "missing";
}

void wait_online(ClusterManager& mgr, const std::string& id) {
    bool ok = wait_for_state(mgr, id, NodeState::Online,
                             std::chrono::milliseconds(10000));
    BOOST_REQUIRE_MESSAGE(
        ok, "peer " << id << " never came online; state=" << state_of(mgr, id));
}

// Generic poll helper for cross-node convergence (route cache hits etc.).
template <typename Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

const char* kMessagingScript = "../tests/lua_api/scripts/messaging_service.lua";

// Spawn a messaging service on `n` under `name`, publishing the extra
// (public) name `alias` when non-empty.
shield::lua::SpawnResult spawn_messaging(Node& n, const std::string& name,
                                         const std::string& alias) {
    nlohmann::json opts = {{"name", name},
                           {"args", nlohmann::json::object()},
                           {"config", nlohmann::json::object()}};
    if (!alias.empty()) {
        opts["config"]["register_alias"] = alias;
    }
    return n.services->spawn(kMessagingScript, opts.dump());
}

// Wire the M4 data plane exactly like the bootstrap glue does: remote-send
// leaves through the transport, inbound envelopes dispatch into the service
// manager, proxied-call completions reply over the transport, and service
// name changes become cluster routes. (The bootstrap itself cannot be
// reused here — it reads process-global state.)
void attach_data_plane(Node& n) {
    // Idempotent; registers the shield_lua CAF block the service manager
    // needs (core/io/cluster blocks are already registered by make_node).
    initialize_caf_types();
    n.runtime = std::make_shared<LuaRuntime>();
    n.services = std::make_shared<LuaServiceManager>(*n.runtime, *n.system);

    auto* mgr = n.manager.get();
    n.services->set_name_change_notifier(
        [mgr](const std::string& name, const std::string& service_id) {
            mgr->on_local_route_changed(name, service_id);
        });

    auto services = n.services;
    ClusterTransport* transport = n.transport.get();
    n.manager->set_remote_send_fn(
        [transport](const std::string& node, const std::string& service_id,
                    const std::string& method, const std::string& args_json,
                    uint64_t call_session, int32_t timeout_ms,
                    std::string* error) {
            return transport->send_envelope(node, service_id, method, args_json,
                                            call_session, timeout_ms, error);
        });

    shield::cluster::EnvelopeBridges bridges;
    bridges.send_dispatch = [services](const std::string& service_id,
                                       const std::string& method,
                                       const std::string& args_json) {
        nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
        if (args.is_discarded()) args = nlohmann::json::array();
        std::string error;
        return services->send(service_id, method, args, &error);
    };
    bridges.call_begin = [services](int32_t timeout_ms) {
        return services->begin_proxied_call(timeout_ms);
    };
    bridges.call_dispatch = [services](uint64_t session,
                                       const std::string& service_id,
                                       const std::string& method,
                                       const std::string& args_json,
                                       std::string* error) {
        nlohmann::json args = nlohmann::json::parse(args_json, nullptr, false);
        if (args.is_discarded()) args = nlohmann::json::array();
        return services->dispatch_proxied_call(session, service_id, method,
                                               args, error);
    };
    bridges.reply_handler = [services](uint64_t call_session, bool ok,
                                       const std::string& payload_json,
                                       const std::string& error_code,
                                       const std::string& error_message) {
        nlohmann::json values;
        if (ok) {
            values = nlohmann::json::parse(payload_json, nullptr, false);
            if (values.is_discarded()) {
                ok = false;
                values = nlohmann::json::array({nlohmann::json::object(
                    {{"code", "handler_error"},
                     {"message", "invalid reply payload"}})});
            }
        } else {
            values = nlohmann::json::array({nlohmann::json::object(
                {{"code", error_code.empty() ? "handler_error" : error_code},
                 {"message", error_message}})});
        }
        services->complete_call(call_session, ok, values);
    };
    n.transport->set_envelope_bridges(std::move(bridges));

    n.services->set_proxied_call_hook(
        [transport](uint64_t session, bool ok, const nlohmann::json& values) {
            if (ok) {
                transport->complete_proxied_call(session, true, values.dump(),
                                                 "", "");
                return;
            }
            std::string code = "handler_error";
            std::string message = "call failed";
            if (values.is_array() && !values.empty() &&
                values.front().is_object()) {
                const auto& first = values.front();
                if (first.contains("code") && first["code"].is_string()) {
                    code = first["code"].get<std::string>();
                }
                if (first.contains("message") && first["message"].is_string()) {
                    message = first["message"].get<std::string>();
                }
            }
            transport->complete_proxied_call(session, false, "", code, message);
        });
}

// Deterministic teardown — same order as the bootstrap glue: stop the
// transport first (its actor exits; the bridges that capture `services` are
// cleared under the data mutex), and only then release the service manager
// and runtime. Resetting the runtime while a straggler envelope dispatch or
// proxied completion could still reference it (via the transport-side
// bridges) leaves those closures with a dangling LuaRuntime.
void teardown_node(Node& n) {
    if (n.services) n.services->shutdown_all("test_done");
    if (n.transport) n.transport->stop();
    n.services.reset();
    n.runtime.reset();
    if (n.manager) n.manager->stop();
}

// Wait until node-a's route cache knows where `alias` lives on node-b.
void wait_route(ClusterManager& mgr, const std::string& alias) {
    BOOST_REQUIRE_MESSAGE(
        wait_until([&] { return !mgr.query_remote("node-b", alias).empty(); },
                   std::chrono::milliseconds(5000)),
        "route for " << alias << " never converged on node-a");
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ClusterTransportIT)

BOOST_AUTO_TEST_CASE(TwoNodesHandshakeAndHeartbeatKeepsThemOnline) {
    enable_test_logging();
    uint16_t port_a = 0;
    uint16_t port_b = 0;
    auto [a, b] = make_node_pair(port_a, port_b);

    // Each side adopts the other's real identity (dialer-side hello_ack).
    wait_online(*a->manager, "node-b");
    wait_online(*b->manager, "node-a");

    // Adopted epochs are the peers' real epochs (nonzero).
    BOOST_CHECK_NE(a->manager->find_node("node-b")->epoch, 0u);
    BOOST_CHECK_NE(b->manager->find_node("node-a")->epoch, 0u);

    // Well past the suspect window: only live heartbeats explain a
    // persistent Online state on both sides.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    BOOST_CHECK(a->manager->find_node("node-b")->state == NodeState::Online);
    BOOST_CHECK(b->manager->find_node("node-a")->state == NodeState::Online);

    // Order matters: the transport actors live in the CAF systems, so they
    // must die before the systems (and their managers) do.
    a->transport->stop();
    b->transport->stop();
    a->manager->stop();
    b->manager->stop();
}

BOOST_AUTO_TEST_CASE(PeerDownMarksOfflineAndRestartReconnectsWithNewEpoch) {
    enable_test_logging();
    uint16_t port_a = 0;
    uint16_t port_b = 0;
    auto [a, b] = make_node_pair(port_a, port_b);

    wait_online(*a->manager, "node-b");
    wait_online(*b->manager, "node-a");
    const uint64_t first_epoch = a->manager->find_node("node-b")->epoch;

    // A cached route for node-b must not survive its departure.
    a->manager->register_route("node-b", "room.public", "sid-old");

    // Kill node-b's transport only: the connection drop is definitive
    // offline, and the route cache entry disappears with it.
    b->transport->stop();
    BOOST_CHECK(wait_for_state(*a->manager, "node-b", NodeState::Offline,
                               std::chrono::milliseconds(10000)));
    BOOST_CHECK_EQUAL(a->manager->query_remote("node-b", "room.public"), "");

    // Restart node-b on the same address with a fresh epoch (new manager):
    // node-a's connect loop redials the configured address, re-adopts the
    // announced identity, and the epoch change keeps stale routes out.
    auto reborn = std::make_unique<ClusterManager>(b->config);
    reborn->start();
    auto reborn_transport =
        std::make_unique<ClusterTransport>(*b->system, *reborn, b->config);
    uint16_t bound = 0;
    std::string error;
    BOOST_REQUIRE_MESSAGE(reborn_transport->start(&bound, error),
                          "restarted transport failed: " << error);
    BOOST_CHECK_EQUAL(bound, port_b);

    wait_online(*a->manager, "node-b");
    const uint64_t second_epoch = a->manager->find_node("node-b")->epoch;
    BOOST_CHECK_NE(first_epoch, second_epoch);
    BOOST_CHECK_EQUAL(a->manager->query_remote("node-b", "room.public"), "");

    // The restarted side also learned node-a again (it dials too).
    wait_online(*reborn, "node-a");

    // Tear down in dependency order: transport actors first (their CAF
    // systems and managers must stay alive underneath them).
    reborn_transport->stop();
    a->transport->stop();
    reborn->stop();
    a->manager->stop();
    b->manager->stop();
}

// M3: the full local route table rides every heartbeat and is re-sent right
// after the handshake, so node-b's remote route cache converges to node-a's
// publications — before any heartbeat, on a publish, on a retract, and the
// cache is purged when node-a's connection drops.
BOOST_AUTO_TEST_CASE(RouteTableConvergesAndPurgesOnPeerDown) {
    enable_test_logging();
    uint16_t port_a = 0;
    uint16_t port_b = 0;

    // node-b starts dialing first; node-a publishes a route BEFORE its
    // transport ever comes up, so only the post-handshake RoutesMsg (not a
    // heartbeat tick) can explain an early hit on node-b's side.
    auto [a, b] =
        make_node_pair(port_a, port_b, /*a_first=*/false, [](Node& n) {
            n.manager->on_local_route_changed("room.public", "sid-join");
        });

    wait_online(*a->manager, "node-b");
    wait_online(*b->manager, "node-a");

    BOOST_CHECK(wait_until(
        [&] {
            return b->manager->query_remote("node-a", "room.public") ==
                   "sid-join";
        },
        std::chrono::milliseconds(5000)));
    // Empty local table on b: nothing to find on a's side.
    BOOST_CHECK_EQUAL(a->manager->query_remote("node-b", "room.public"), "");

    // A later publication propagates at heartbeat cadence.
    a->manager->on_local_route_changed("auth.login", "sid-2");
    BOOST_CHECK(wait_until(
        [&] {
            return b->manager->query_remote("node-a", "auth.login") == "sid-2";
        },
        std::chrono::milliseconds(5000)));

    // Retraction propagates the same way: the next full table simply no
    // longer contains the name.
    a->manager->on_local_route_changed("auth.login", "");
    BOOST_CHECK(wait_until(
        [&] {
            return b->manager->query_remote("node-a", "auth.login").empty();
        },
        std::chrono::milliseconds(5000)));
    // ... while the untouched route survives the replace.
    BOOST_CHECK_EQUAL(b->manager->query_remote("node-a", "room.public"),
                      "sid-join");

    // node-a's connection drop is definitive offline and purges b's cache.
    a->transport->stop();
    BOOST_CHECK(wait_for_state(*b->manager, "node-a", NodeState::Offline,
                               std::chrono::milliseconds(10000)));
    BOOST_CHECK_EQUAL(b->manager->query_remote("node-a", "room.public"), "");

    b->transport->stop();
    a->manager->stop();
    b->manager->stop();
}

// -- M4: cross-node send/call over the envelope data plane ------------------

// The full round trip: node-a's caller service shield.call's a service that
// lives on node-b (coroutine path), the main thread sync-calls it through
// call_with_session, a one-way send lands on the callee, and a retracted
// route fails with service_not_found rather than node_offline.
BOOST_AUTO_TEST_CASE(RemoteCallAndSendRoundTripEndToEnd) {
    enable_test_logging();
    uint16_t port_a = 0;
    uint16_t port_b = 0;
    auto [a, b] = make_node_pair(port_a, port_b, /*a_first=*/false);
    attach_data_plane(*a);
    attach_data_plane(*b);

    // shield.send/call resolve remote targets through the process-global
    // cluster manager; in this single-process test only node-a originates
    // remote calls, so the global points at its manager.
    shield::cluster::set_global_cluster_manager(a->manager.get());

    auto callee = spawn_messaging(*b, "echo_impl", "echo_svc");
    BOOST_REQUIRE(callee.success);
    auto caller = spawn_messaging(*a, "caller_impl", "");
    BOOST_REQUIRE(caller.success);

    wait_online(*a->manager, "node-b");
    wait_online(*b->manager, "node-a");
    wait_route(*a->manager, "echo_svc");

    // Coroutine-path call: the caller's handler yields inside shield.call
    // and is resumed when the echo reply arrives over the envelope path.
    auto called = a->services->call(
        caller.service_id, "call_target",
        nlohmann::json::array({"node-b:echo_svc", "echo", "ping"}), 5000);
    BOOST_REQUIRE(called.success);
    BOOST_REQUIRE_EQUAL(called.values.size(), 2u);
    BOOST_REQUIRE_MESSAGE(called.values[0].get<bool>() == true,
                          "remote call failed: " << called.values[1].dump());
    BOOST_CHECK_EQUAL(called.values[1].get<std::string>(), "ping");

    // Main-thread sync call: the same round trip through
    // LuaServiceManager::call_with_session.
    const std::string remote_sid =
        a->manager->query_remote("node-b", "echo_svc");
    std::string send_err;
    auto sync_result = a->services->call_with_session(
        [&](uint64_t session, std::string& err) {
            return a->manager->send_remote(
                "node-b", remote_sid, "echo",
                nlohmann::json::array({"direct"}).dump(), session, 3000, &err);
        },
        3000);
    BOOST_REQUIRE(sync_result.success);
    BOOST_REQUIRE_EQUAL(sync_result.values.size(), 1u);
    BOOST_CHECK_EQUAL(sync_result.values[0].get<std::string>(), "direct");

    // One-way send: the callee records the payload under its published name.
    BOOST_CHECK(a->manager->send_remote(
        "node-b", remote_sid, "record",
        nlohmann::json::array({"one-way"}).dump(), 0, 0, &send_err));
    BOOST_CHECK(wait_until(
        [&] {
            auto seen = b->services->call(remote_sid, "get_last_args",
                                          nlohmann::json::array(), 2000);
            return seen.success && !seen.values.empty() &&
                   seen.values[0].is_array() && !seen.values[0].empty() &&
                   seen.values[0][0] == "one-way";
        },
        std::chrono::milliseconds(5000)));

    // Retraction: once node-b unpublishes the name, remote calls fail with
    // service_not_found (route gone) while the node itself stays reachable.
    auto unreg = b->services->call(remote_sid, "unregister_name",
                                   nlohmann::json::array({"echo_svc"}), 2000);
    BOOST_REQUIRE(unreg.success);
    BOOST_CHECK(wait_until(
        [&] { return a->manager->query_remote("node-b", "echo_svc").empty(); },
        std::chrono::milliseconds(5000)));
    auto after_retract = a->services->call(
        caller.service_id, "call_timeout_target",
        nlohmann::json::array({1000, "node-b:echo_svc", "echo", "x"}), 5000);
    BOOST_REQUIRE(after_retract.success);
    BOOST_CHECK_EQUAL(after_retract.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(after_retract.values[1]["code"].get<std::string>(),
                      "service_not_found");

    shield::cluster::set_global_cluster_manager(nullptr);
    teardown_node(*a);
    teardown_node(*b);
}

// Losing the peer's connection is definitive: the route purges, the node
// degrades to Offline, and a remote call fails immediately with node_offline
// instead of hanging until its timeout.
BOOST_AUTO_TEST_CASE(RemoteCallFailsFastWhenPeerTransportStops) {
    enable_test_logging();
    uint16_t port_a = 0;
    uint16_t port_b = 0;
    auto [a, b] = make_node_pair(port_a, port_b, /*a_first=*/false);
    attach_data_plane(*a);
    attach_data_plane(*b);
    shield::cluster::set_global_cluster_manager(a->manager.get());

    auto callee = spawn_messaging(*b, "echo_impl", "echo_svc");
    BOOST_REQUIRE(callee.success);
    auto caller = spawn_messaging(*a, "caller_impl", "");
    BOOST_REQUIRE(caller.success);

    wait_online(*a->manager, "node-b");
    wait_online(*b->manager, "node-a");
    wait_route(*a->manager, "echo_svc");

    b->transport->stop();
    BOOST_CHECK(wait_for_state(*a->manager, "node-b", NodeState::Offline,
                               std::chrono::milliseconds(10000)));
    BOOST_CHECK_EQUAL(a->manager->query_remote("node-b", "echo_svc"), "");

    const auto started = std::chrono::steady_clock::now();
    auto failed = a->services->call(
        caller.service_id, "call_timeout_target",
        nlohmann::json::array({5000, "node-b:echo_svc", "echo", "x"}), 10000);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    BOOST_REQUIRE(failed.success);
    BOOST_CHECK_EQUAL(failed.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(failed.values[1]["code"].get<std::string>(),
                      "node_offline");
    // Fast-fail, not a 5s ride to the caller's timeout.
    BOOST_CHECK(elapsed < std::chrono::milliseconds(3000));

    shield::cluster::set_global_cluster_manager(nullptr);
    teardown_node(*a);
    teardown_node(*b);
}

// A slow callee rides out the caller's budget: the local timeout driver
// resumes the suspended caller with the stable timeout error while the
// callee's late completion lands harmlessly on an already-expired session.
BOOST_AUTO_TEST_CASE(RemoteCallTimesOutWhileCalleeIsSlow) {
    enable_test_logging();
    uint16_t port_a = 0;
    uint16_t port_b = 0;
    auto [a, b] = make_node_pair(port_a, port_b, /*a_first=*/false);
    attach_data_plane(*a);
    attach_data_plane(*b);
    shield::cluster::set_global_cluster_manager(a->manager.get());

    auto callee = spawn_messaging(*b, "echo_impl", "echo_svc");
    BOOST_REQUIRE(callee.success);
    auto caller = spawn_messaging(*a, "caller_impl", "");
    BOOST_REQUIRE(caller.success);

    wait_online(*a->manager, "node-b");
    wait_online(*b->manager, "node-a");
    wait_route(*a->manager, "echo_svc");

    // slow_method sleeps 150ms on the callee; the caller only grants 50ms.
    auto timed_out = a->services->call(
        caller.service_id, "call_timeout_target",
        nlohmann::json::array({50, "node-b:echo_svc", "slow_method"}), 5000);
    BOOST_REQUIRE(timed_out.success);
    BOOST_CHECK_EQUAL(timed_out.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(timed_out.values[1]["code"].get<std::string>(),
                      "timeout");

    // Give the callee's late completion time to arrive (it must no-op on
    // node-a), then a fresh call still works end to end.
    auto again = a->services->call(
        caller.service_id, "call_target",
        nlohmann::json::array({"node-b:echo_svc", "echo", "after"}), 5000);
    BOOST_REQUIRE(again.success);
    BOOST_CHECK_EQUAL(again.values[0].get<bool>(), true);
    BOOST_CHECK_EQUAL(again.values[1].get<std::string>(), "after");

    shield::cluster::set_global_cluster_manager(nullptr);
    teardown_node(*a);
    teardown_node(*b);
}

BOOST_AUTO_TEST_SUITE_END()
