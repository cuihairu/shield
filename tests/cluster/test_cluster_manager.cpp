// ClusterManager state-machine unit tests (M1 + M2 seams).
// Constructed directly from ClusterConfig: no global config, no network,
// no bootstrap. Timing tests use short windows with generous poll budgets.
#define BOOST_TEST_MODULE ClusterManagerTests
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "shield/cluster/cluster_manager.hpp"

using shield::cluster::ClusterConfig;
using shield::cluster::ClusterManager;
using shield::cluster::node_state_name;
using shield::cluster::NodeState;

namespace {

ClusterConfig two_peer_config() {
    ClusterConfig cfg;
    cfg.enabled = true;
    cfg.node_id = "test-node";
    cfg.listen_address = "127.0.0.1:19999";
    cfg.peers = {"127.0.0.1:20001", "127.0.0.1:20002"};
    cfg.heartbeat_interval_ms = 5000;
    cfg.suspect_timeout_ms = 15000;
    cfg.offline_timeout_ms = 30000;
    return cfg;
}

// Peers are keyed by address until a handshake learns the real node_id.
const std::string kPeerA = "127.0.0.1:20001";
const std::string kPeerB = "127.0.0.1:20002";

bool wait_for_state(ClusterManager& mgr, const std::string& key,
                    NodeState expected, std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto* node = mgr.find_node(key);
        if (node && node->state == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(ClusterState)

BOOST_AUTO_TEST_CASE(ParseRemoteTargetAcceptsNodeIdServicePairs) {
    std::string node, service;
    BOOST_CHECK(ClusterManager::parse_remote_target("node-a:room.public", node,
                                                    service));
    BOOST_CHECK_EQUAL(node, "node-a");
    BOOST_CHECK_EQUAL(service, "room.public");

    // Only the first colon splits; service names may contain colons.
    BOOST_CHECK(
        ClusterManager::parse_remote_target("n:svc:extra", node, service));
    BOOST_CHECK_EQUAL(node, "n");
    BOOST_CHECK_EQUAL(service, "svc:extra");
}

BOOST_AUTO_TEST_CASE(ParseRemoteTargetRejectsMalformedTargets) {
    std::string node, service;
    BOOST_CHECK(
        !ClusterManager::parse_remote_target("no-colon", node, service));
    BOOST_CHECK(
        !ClusterManager::parse_remote_target(":service", node, service));
    BOOST_CHECK(!ClusterManager::parse_remote_target("node:", node, service));
    BOOST_CHECK(!ClusterManager::parse_remote_target(std::string_view(""), node,
                                                     service));
}

BOOST_AUTO_TEST_CASE(TickBeforeStartIsNoop) {
    ClusterManager mgr(two_peer_config());
    BOOST_CHECK_EQUAL(mgr.tick(), 0);
    // Peers exist in the Connecting placeholder state before start().
    const auto* node = mgr.find_node(kPeerA);
    BOOST_REQUIRE(node);
    BOOST_CHECK(node->state == NodeState::Connecting);
    BOOST_CHECK_EQUAL(node_state_name(node->state), "connecting");
    BOOST_CHECK_EQUAL(mgr.check_node_reachable(kPeerA), "");
}

BOOST_AUTO_TEST_CASE(StartKeepsPeersConnectingUntilHandshake) {
    // M2: start() no longer fakes Online — the transport's handshake does.
    // With no transport, peers stay Connecting (reachable placeholder) until
    // they either complete the handshake or the offline window lapses.
    auto cfg = two_peer_config();
    cfg.suspect_timeout_ms = 50;
    cfg.offline_timeout_ms = 150;
    ClusterManager mgr(cfg);

    mgr.start();
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Connecting);

    // Handshake completes: identity adoption renames the entry under the
    // announced node_id and marks it Online.
    mgr.on_handshake(kPeerA, "node-a", 7);
    mgr.on_handshake(kPeerB, "node-b", 8);
    BOOST_CHECK(mgr.find_node("node-a")->state == NodeState::Online);
    BOOST_CHECK(mgr.find_node("node-b")->state == NodeState::Online);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("node-a"), "");
    BOOST_CHECK(!mgr.find_node(kPeerA));  // placeholder key is gone

    // Past suspect_timeout but before offline_timeout: exactly one
    // degradation per peer, and the reachable error reflects it.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(mgr.tick(), 2);
    BOOST_CHECK(mgr.find_node("node-a")->state == NodeState::Suspect);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("node-a"), "node_suspect");

    // Past offline_timeout: second degradation, then steady state.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(mgr.tick(), 2);
    BOOST_CHECK(mgr.find_node("node-a")->state == NodeState::Offline);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("node-a"), "node_offline");
    BOOST_CHECK_EQUAL(mgr.tick(), 0);

    // An unknown node is distinct from a known-but-degraded one.
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("never-configured"),
                      "node_not_found");

    mgr.stop();
}

BOOST_AUTO_TEST_CASE(ConnectingPeerDegradesToOfflineAfterHandshakeTimeout) {
    // A configured peer that never completes a handshake is honest debris:
    // once the offline window passes with no transport success, tick() flips
    // Connecting -> Offline directly (no suspect grace for a node we never
    // really talked to).
    auto cfg = two_peer_config();
    cfg.offline_timeout_ms = 50;
    ClusterManager mgr(cfg);

    mgr.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    BOOST_CHECK_EQUAL(mgr.tick(), 2);
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Offline);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable(kPeerA), "node_offline");
    mgr.stop();
}

BOOST_AUTO_TEST_CASE(HeartbeatThreadDrivesDegradation) {
    // No manual tick() calls: the scheduler thread started by start() must
    // degrade peers on its own. Peers that never handshake degrade straight
    // to Offline; an adopted peer goes through the suspect ladder.
    auto cfg = two_peer_config();
    cfg.heartbeat_interval_ms = 20;  // clamped to >= 50 internally
    cfg.suspect_timeout_ms = 60;
    cfg.offline_timeout_ms = 120;
    ClusterManager mgr(cfg);

    mgr.start();
    BOOST_CHECK(wait_for_state(mgr, kPeerA, NodeState::Offline,
                               std::chrono::milliseconds(2000)));
    BOOST_CHECK(wait_for_state(mgr, kPeerB, NodeState::Offline,
                               std::chrono::milliseconds(2000)));

    // A late handshake still adopts the identity and restores Online; the
    // placeholder entry disappears with the rename.
    mgr.on_handshake(kPeerA, "node-a", 5);
    BOOST_CHECK(mgr.find_node("node-a")->state == NodeState::Online);
    BOOST_CHECK(!mgr.find_node(kPeerA));

    // ... and the running scheduler degrades it again once heartbeats stop.
    BOOST_CHECK(wait_for_state(mgr, "node-a", NodeState::Suspect,
                               std::chrono::milliseconds(2000)));

    // stop() must join the scheduler without hanging; after it, the
    // peer states are torn down to Removed.
    mgr.stop();
    BOOST_CHECK(mgr.find_node("node-a")->state == NodeState::Removed);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("node-a"), "node_removed");
    // tick() after stop stays a no-op.
    BOOST_CHECK_EQUAL(mgr.tick(), 0);
}

BOOST_AUTO_TEST_CASE(DoubleStopAndDestructorStopAreSafe) {
    auto cfg = two_peer_config();
    cfg.heartbeat_interval_ms = 20;
    {
        ClusterManager mgr(cfg);
        mgr.start();
        mgr.stop();
        mgr.stop();  // second stop is a no-op
    }  // destructor runs stop() again on a stopped manager

    // Restart after a full stop is supported: peers are tracked again (in
    // the Connecting placeholder state) with a fresh heartbeat scheduler,
    // and stop() tears them down again.
    ClusterManager mgr(cfg);
    mgr.start();
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Connecting);
    mgr.stop();
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Removed);
    mgr.stop();
}

BOOST_AUTO_TEST_CASE(RouteCacheRoundtrip) {
    ClusterManager mgr(two_peer_config());
    BOOST_CHECK_EQUAL(mgr.query_remote("node-b", "room.public"), "");
    mgr.register_route("node-b", "room.public", "sid-42");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-b", "room.public"), "sid-42");
    // Same name on another node is a separate cache entry.
    BOOST_CHECK_EQUAL(mgr.query_remote("node-c", "room.public"), "");
    // Re-registering overwrites.
    mgr.register_route("node-b", "room.public", "sid-43");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-b", "room.public"), "sid-43");
}

BOOST_AUTO_TEST_CASE(SendRemoteUsesInjectedFunction) {
    ClusterManager mgr(two_peer_config());
    // Without an injected transport the send fails.
    BOOST_CHECK(!mgr.send_remote("node-b", "sid-1", "on_ping", "[]"));

    std::string got_node, got_service, got_method, got_args;
    mgr.set_remote_send_fn(
        [&](const std::string& node, const std::string& service,
            const std::string& method, const std::string& args_json) {
            got_node = node;
            got_service = service;
            got_method = method;
            got_args = args_json;
            return true;
        });
    BOOST_CHECK(mgr.send_remote("node-b", "sid-1", "on_ping", R"(["a",1])"));
    BOOST_CHECK_EQUAL(got_node, "node-b");
    BOOST_CHECK_EQUAL(got_service, "sid-1");
    BOOST_CHECK_EQUAL(got_method, "on_ping");
    BOOST_CHECK_EQUAL(got_args, R"(["a",1])");

    // A transport-level failure propagates as false.
    mgr.set_remote_send_fn([](const std::string&, const std::string&,
                              const std::string&,
                              const std::string&) { return false; });
    BOOST_CHECK(!mgr.send_remote("node-b", "sid-1", "on_ping", "[]"));
}

BOOST_AUTO_TEST_CASE(NodeEpochIsRandomAndStable) {
    ClusterManager first(two_peer_config());
    ClusterManager second(two_peer_config());
    BOOST_CHECK(first.node_epoch() != 0);
    BOOST_CHECK(first.node_epoch() == first.node_epoch());
    // Distinct managers draw distinct epochs.
    BOOST_CHECK(first.node_epoch() != second.node_epoch());
}

BOOST_AUTO_TEST_CASE(NodesSnapshotCarriesPeerAddresses) {
    ClusterManager mgr(two_peer_config());
    const auto snapshot = mgr.nodes();
    BOOST_CHECK_EQUAL(snapshot.size(), 2u);
    bool saw_a = false, saw_b = false;
    for (const auto& node : snapshot) {
        saw_a = saw_a || node.address == kPeerA;
        saw_b = saw_b || node.address == kPeerB;
        BOOST_CHECK_EQUAL(node.node_id, node.address);  // placeholder id
        BOOST_CHECK(node.epoch == 0);  // learned only via handshake (M2)
    }
    BOOST_CHECK(saw_a && saw_b);
    BOOST_CHECK_EQUAL(mgr.node_id(), "test-node");
}

BOOST_AUTO_TEST_CASE(HandshakeAdoptsIdentity) {
    ClusterManager mgr(two_peer_config());
    BOOST_CHECK(!mgr.find_node("node-a"));  // unknown before handshake

    mgr.on_handshake(kPeerA, "node-a", 42);
    const auto* adopted = mgr.find_node("node-a");
    BOOST_REQUIRE(adopted);
    BOOST_CHECK(adopted->state == NodeState::Online);
    BOOST_CHECK_EQUAL(adopted->epoch, 42u);
    BOOST_CHECK_EQUAL(adopted->address, kPeerA);
    BOOST_CHECK(!mgr.find_node(kPeerA));  // renamed away from the placeholder
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("node-a"), "");

    // Handshake for an address that is not a configured peer is ignored.
    mgr.on_handshake("10.0.0.1:1", "rogue", 1);
    BOOST_CHECK(!mgr.find_node("rogue"));
}

BOOST_AUTO_TEST_CASE(RehandshakeWithNewEpochClearsRoutes) {
    ClusterManager mgr(two_peer_config());
    mgr.on_handshake(kPeerA, "node-a", 1);
    mgr.register_route("node-a", "room.public", "sid-1");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "sid-1");

    // Same epoch (idempotent retry): routes survive.
    mgr.on_handshake(kPeerA, "node-a", 1);
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "sid-1");

    // New epoch means the peer restarted: its routes are stale.
    mgr.on_handshake(kPeerA, "node-a", 2);
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "");
    BOOST_CHECK_EQUAL(mgr.find_node("node-a")->epoch, 2u);
    BOOST_CHECK(mgr.find_node("node-a")->state == NodeState::Online);
}

BOOST_AUTO_TEST_CASE(HeartbeatRestoresSuspectNode) {
    auto cfg = two_peer_config();
    cfg.suspect_timeout_ms = 50;
    cfg.offline_timeout_ms = 5000;
    ClusterManager mgr(cfg);

    // tick() is gated on start(): without it the state machine is frozen.
    mgr.start();
    mgr.on_handshake(kPeerB, "node-b", 9);
    BOOST_CHECK(mgr.find_node("node-b")->state == NodeState::Online);

    // Let the heartbeat clock lapse: ticks (ours or the scheduler's) flip
    // Online -> Suspect. Poll instead of asserting an exact change count —
    // the background scheduler's first tick may land anywhere in here.
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
        mgr.tick();
        if (mgr.find_node("node-b")->state == NodeState::Suspect) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_CHECK(mgr.find_node("node-b")->state == NodeState::Suspect);

    // A heartbeat restores Online and restarts the clock.
    mgr.on_heartbeat("node-b");
    BOOST_CHECK(mgr.find_node("node-b")->state == NodeState::Online);
    BOOST_CHECK_EQUAL(mgr.tick(), 0);

    // Heartbeats from unknown nodes neither crash nor register anything.
    mgr.on_heartbeat("ghost");
    BOOST_CHECK(!mgr.find_node("ghost"));

    mgr.stop();
}

BOOST_AUTO_TEST_CASE(PeerDownClearsRoutesAndMarksOffline) {
    ClusterManager mgr(two_peer_config());
    mgr.on_handshake(kPeerA, "node-a", 3);
    mgr.register_route("node-a", "room.public", "sid-9");

    mgr.on_peer_down(kPeerA);
    const auto* node = mgr.find_node("node-a");
    BOOST_REQUIRE(node);
    // Definitive drop: no suspect grace for a closed connection.
    BOOST_CHECK(node->state == NodeState::Offline);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("node-a"), "node_offline");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "");

    // Repeated and unknown drops are no-ops.
    mgr.on_peer_down(kPeerA);
    mgr.on_peer_down("10.0.0.9:1");
    BOOST_CHECK(mgr.find_node("node-a")->state == NodeState::Offline);
    BOOST_CHECK_EQUAL(mgr.nodes().size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// M3 route learning: the local publication table (shipped to peers at
// heartbeat cadence) and the epoch-validated remote route cache.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_SUITE(ClusterRoutes)

BOOST_AUTO_TEST_CASE(LocalRoutePublishRetractAndSnapshot) {
    ClusterManager mgr(two_peer_config());
    BOOST_CHECK(mgr.local_routes().empty());

    mgr.on_local_route_changed("room.public", "sid-1");
    mgr.on_local_route_changed("auth.login", "sid-2");
    auto routes = mgr.local_routes();
    BOOST_CHECK_EQUAL(routes.size(), 2u);
    bool saw_room = false, saw_auth = false;
    for (const auto& [name, service_id] : routes) {
        saw_room = saw_room || (name == "room.public" && service_id == "sid-1");
        saw_auth = saw_auth || (name == "auth.login" && service_id == "sid-2");
    }
    BOOST_CHECK(saw_room && saw_auth);

    // Re-publish overwrites; empty service_id retracts.
    mgr.on_local_route_changed("room.public", "sid-3");
    mgr.on_local_route_changed("auth.login", "");
    routes = mgr.local_routes();
    BOOST_CHECK_EQUAL(routes.size(), 1u);
    BOOST_CHECK_EQUAL(routes[0].first, "room.public");
    BOOST_CHECK_EQUAL(routes[0].second, "sid-3");

    // Retracting a name that was never published is a no-op.
    mgr.on_local_route_changed("ghost", "");
    BOOST_CHECK_EQUAL(mgr.local_routes().size(), 1u);
}

BOOST_AUTO_TEST_CASE(OnRoutesReplacesBucketAndValidatesEpoch) {
    ClusterManager mgr(two_peer_config());

    // Routes from an identity we never adopted are dropped.
    mgr.on_routes("node-a", 1, {{"room.public", "sid-1"}});
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "");

    // Adopted node + matching epoch: the table lands.
    mgr.on_handshake(kPeerA, "node-a", 1);
    mgr.on_routes("node-a", 1,
                  {{"room.public", "sid-1"}, {"extra.svc", "sid-2"}});
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "sid-1");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "extra.svc"), "sid-2");

    // A stale epoch (table from a dead instance) is dropped wholesale; the
    // live instance's table survives untouched.
    mgr.on_routes("node-a", 0, {{"room.public", "stale"}});
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "sid-1");

    // Full-bucket replace: names absent from the new table disappear.
    mgr.on_routes("node-a", 1, {{"room.public", "sid-new"}});
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "sid-new");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "extra.svc"), "");

    // Empty service_id entries are filtered out, the rest still land.
    mgr.on_routes("node-a", 1, {{"room.public", ""}, {"keep.svc", "sid-4"}});
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "room.public"), "");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-a", "keep.svc"), "sid-4");
}

BOOST_AUTO_TEST_CASE(EarlyRoutesUnderPlaceholderKeyDoNotLeak) {
    // RoutesMsg only ever travels post-handshake on the wire, but the
    // receiver's guard is identity + epoch, and a pre-adoption placeholder
    // entry has epoch 0. A table stored under the placeholder key must not
    // survive the rename to the announced identity.
    ClusterManager mgr(two_peer_config());
    mgr.on_routes(kPeerB, 0, {{"early.svc", "sid-5"}});
    BOOST_CHECK_EQUAL(mgr.query_remote(kPeerB, "early.svc"), "sid-5");

    // Adoption erases the placeholder-keyed cache before renaming.
    mgr.on_handshake(kPeerB, "node-b", 5);
    BOOST_CHECK_EQUAL(mgr.query_remote(kPeerB, "early.svc"), "");
    BOOST_CHECK_EQUAL(mgr.query_remote("node-b", "early.svc"), "");
}

BOOST_AUTO_TEST_SUITE_END()
