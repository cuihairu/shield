// ClusterManager state-machine unit tests (M1).
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

BOOST_AUTO_TEST_CASE(StartMarksPeersOnlineAndTickDegradesThem) {
    auto cfg = two_peer_config();
    cfg.suspect_timeout_ms = 50;
    cfg.offline_timeout_ms = 150;
    ClusterManager mgr(cfg);

    mgr.start();
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Online);
    BOOST_CHECK(mgr.find_node(kPeerB)->state == NodeState::Online);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable(kPeerA), "");

    // Past suspect_timeout but before offline_timeout: exactly one
    // degradation per peer, and the reachable error reflects it.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(mgr.tick(), 2);
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Suspect);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable(kPeerA), "node_suspect");

    // Past offline_timeout: second degradation, then steady state.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK_EQUAL(mgr.tick(), 2);
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Offline);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable(kPeerA), "node_offline");
    BOOST_CHECK_EQUAL(mgr.tick(), 0);

    // An unknown node is distinct from a known-but-degraded one.
    BOOST_CHECK_EQUAL(mgr.check_node_reachable("never-configured"),
                      "node_not_found");

    mgr.stop();
}

BOOST_AUTO_TEST_CASE(HeartbeatThreadDrivesDegradation) {
    // No manual tick() calls: the scheduler thread started by start() must
    // degrade peers on its own.
    auto cfg = two_peer_config();
    cfg.heartbeat_interval_ms = 20;  // clamped to >= 50 internally
    cfg.suspect_timeout_ms = 60;
    cfg.offline_timeout_ms = 120;
    ClusterManager mgr(cfg);

    mgr.start();
    BOOST_CHECK(wait_for_state(mgr, kPeerA, NodeState::Suspect,
                               std::chrono::milliseconds(2000)));
    BOOST_CHECK(wait_for_state(mgr, kPeerB, NodeState::Offline,
                               std::chrono::milliseconds(2000)));

    // stop() must join the scheduler without hanging; after it, the
    // peer states are torn down to Removed.
    mgr.stop();
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Removed);
    BOOST_CHECK_EQUAL(mgr.check_node_reachable(kPeerA), "node_removed");
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

    // Restart after a full stop is supported: peers come back online with a
    // fresh heartbeat scheduler, and stop() tears them down again.
    ClusterManager mgr(cfg);
    mgr.start();
    BOOST_CHECK(mgr.find_node(kPeerA)->state == NodeState::Online);
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

BOOST_AUTO_TEST_SUITE_END()
