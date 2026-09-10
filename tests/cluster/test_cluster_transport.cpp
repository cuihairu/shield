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
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#include "shield/log/logger.hpp"

using shield::cluster::ClusterConfig;
using shield::cluster::ClusterManager;
using shield::cluster::ClusterTransport;
using shield::cluster::node_state_name;
using shield::cluster::NodeState;

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

// Reserve a loopback port and hand it back. There is a tiny race between
// close() and the CAF listener bind, acceptable for tests.
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
};

std::unique_ptr<Node> make_node(const std::string& node_id, uint16_t listen,
                                uint16_t peer_port) {
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
    node->transport = std::make_unique<ClusterTransport>(
        *node->system, *node->manager, node->config);
    uint16_t bound = 0;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node->transport->start(&bound, error),
                          "transport start failed: " << error);
    BOOST_CHECK_EQUAL(bound, listen);
    return node;
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

}  // namespace

BOOST_AUTO_TEST_SUITE(ClusterTransportIT)

BOOST_AUTO_TEST_CASE(TwoNodesHandshakeAndHeartbeatKeepsThemOnline) {
    enable_test_logging();
    uint16_t port_a = free_port();
    uint16_t port_b = free_port();
    auto a = make_node("node-a", port_a, port_b);
    auto b = make_node("node-b", port_b, port_a);

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
    uint16_t port_a = free_port();
    uint16_t port_b = free_port();
    auto a = make_node("node-a", port_a, port_b);
    auto b = make_node("node-b", port_b, port_a);

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

BOOST_AUTO_TEST_SUITE_END()
