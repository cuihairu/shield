// NodeDiscovery tests: loop semantics against a fake backend (no Redis),
// manager integration (discovery feeds ClusterManager), and the real
// redis++ backend against a self-spawned redis-server (random free port,
// one per case — no fixed ports, no external state).
#define BOOST_TEST_MODULE NodeDiscoveryTests
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <csignal>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/node_discovery.hpp"
#include "shield/config/config.hpp"

using namespace shield::cluster;
using namespace std::chrono_literals;

namespace {

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

// ---------------------------------------------------------------------------
// Fake backend: in-memory node map + failure switches so tests can drive
// every error arm of the loop.
// ---------------------------------------------------------------------------
class FakeBackend final : public NodeDiscoveryBackend {
public:
    void register_node(const std::string& node_id, const std::string& address,
                       int) override {
        if (fail_register) throw std::runtime_error("register fail");
        {
            std::lock_guard<std::mutex> lock(mu);
            nodes_[node_id] = address;
        }
        ++registers;
    }
    void heartbeat(const std::string& node_id, int) override {
        if (fail_heartbeat) throw std::runtime_error("heartbeat fail");
        std::lock_guard<std::mutex> lock(mu);
        if (!nodes_.count(node_id)) {
            throw std::runtime_error("heartbeat for unregistered node");
        }
        ++heartbeats;
    }
    std::vector<DiscoveredNode> scan(const std::string& self_id) override {
        if (fail_scan) throw std::runtime_error("scan fail");
        std::lock_guard<std::mutex> lock(mu);
        std::vector<DiscoveredNode> out;
        for (const auto& [id, addr] : nodes_) {
            if (id != self_id) out.push_back(DiscoveredNode{id, addr});
        }
        return out;
    }
    void unregister_node(const std::string& node_id) override {
        if (fail_unregister) throw std::runtime_error("unregister fail");
        std::lock_guard<std::mutex> lock(mu);
        nodes_.erase(node_id);
        ++unregisters;
    }

    std::mutex mu;
    std::map<std::string, std::string> nodes_;
    std::atomic<int> registers{0};
    std::atomic<int> heartbeats{0};
    std::atomic<int> unregisters{0};
    std::atomic<bool> fail_register{false};
    std::atomic<bool> fail_heartbeat{false};
    std::atomic<bool> fail_scan{false};
    std::atomic<bool> fail_unregister{false};

    void seed(const std::string& id, const std::string& addr) {
        std::lock_guard<std::mutex> lock(mu);
        nodes_[id] = addr;
    }
    void remove(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        nodes_.erase(id);
    }
    bool contains(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        return nodes_.count(id) > 0;
    }
};

// Callback event sink.
struct Events {
    std::mutex mu;
    std::vector<std::string> discovered;  // "id@addr"
    std::vector<std::string> lost;

    void on_discovered(const DiscoveredNode& n) {
        std::lock_guard<std::mutex> lock(mu);
        discovered.push_back(n.node_id + "@" + n.address);
    }
    void on_lost(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        lost.push_back(id);
    }
    bool saw_discovered(const std::string& what) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& s : discovered) {
            if (s.find(what) != std::string::npos) return true;
        }
        return false;
    }
    bool saw_lost(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& s : lost) {
            if (s == id) return true;
        }
        return false;
    }
};

// The discovery loop owns its backend (unique_ptr). Tests keep shared
// ownership through this forwarding adapter so they can still inspect the
// fake after the loop (or the manager that destroyed it) is gone.
class SharedBackendHandle final : public NodeDiscoveryBackend {
public:
    explicit SharedBackendHandle(std::shared_ptr<NodeDiscoveryBackend> impl)
        : impl_(std::move(impl)) {}
    void register_node(const std::string& node_id, const std::string& address,
                       int ttl_seconds) override {
        impl_->register_node(node_id, address, ttl_seconds);
    }
    void heartbeat(const std::string& node_id, int ttl_seconds) override {
        impl_->heartbeat(node_id, ttl_seconds);
    }
    std::vector<DiscoveredNode> scan(const std::string& self_id) override {
        return impl_->scan(self_id);
    }
    void unregister_node(const std::string& node_id) override {
        impl_->unregister_node(node_id);
    }

private:
    std::shared_ptr<NodeDiscoveryBackend> impl_;
};

std::unique_ptr<NodeDiscovery> make_loop(
    const std::string& id, std::shared_ptr<NodeDiscoveryBackend> backend,
    Events& events, int interval_ms = 2) {
    auto loop = std::make_unique<NodeDiscovery>(
        id, "127.0.0.1:21099",
        std::make_unique<SharedBackendHandle>(std::move(backend)), interval_ms,
        interval_ms, 5);
    loop->set_callbacks(
        [&events](const DiscoveredNode& n) { events.on_discovered(n); },
        [&events](const std::string& id) { events.on_lost(id); });
    return loop;
}

// ---------------------------------------------------------------------------
// Real redis-server helper (POSIX only; the cluster CI legs are Linux).
// ---------------------------------------------------------------------------
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int probe_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool tcp_accepts(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok =
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
}

struct RedisServer {
    int port = -1;
    pid_t pid = -1;
    ~RedisServer() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

// Spawn a throwaway redis-server on a random free port; nullptr when the
// binary is unavailable (CI installs redis-server on the cluster legs).
std::unique_ptr<RedisServer> spawn_redis_server() {
    auto srv = std::make_unique<RedisServer>();
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int port = probe_free_port();
        if (port <= 0) return nullptr;
        const pid_t pid = ::fork();
        if (pid < 0) return nullptr;
        if (pid == 0) {
            const int devnull = ::open("/dev/null", O_WRONLY);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            const auto port_str = std::to_string(port);
            ::execlp("redis-server", "redis-server", "--port", port_str.c_str(),
                     "--save", "", "--appendonly", "no",
                     static_cast<char*>(nullptr));
            _exit(127);
        }
        // Readiness: poll until the port accepts.
        for (int i = 0; i < 100 && !tcp_accepts(port); ++i) {
            std::this_thread::sleep_for(20ms);
        }
        if (tcp_accepts(port)) {
            srv->port = port;
            srv->pid = pid;
            return srv;
        }
        ::kill(pid, SIGTERM);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
    return nullptr;
}

RedisDiscoveryConfig redis_cfg(const RedisServer& srv,
                               const std::string& prefix, int ttl = 10) {
    RedisDiscoveryConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = srv.port;
    cfg.prefix = prefix;
    cfg.ttl_seconds = ttl;
    return cfg;
}
#endif  // !_WIN32

}  // namespace

// ---------------------------------------------------------------------------
// Loop semantics (fake backend)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(LoopRegistersHeartbeatsUnregisters) {
    auto backend = std::make_shared<FakeBackend>();
    Events events;
    auto loop = make_loop("self-a", backend, events);
    loop->start();
    BOOST_CHECK(wait_until([&] { return backend->registers >= 1; }, 2000ms));
    BOOST_CHECK(backend->contains("self-a"));
    {
        std::lock_guard<std::mutex> lock(backend->mu);
        BOOST_CHECK_EQUAL(backend->nodes_["self-a"], "127.0.0.1:21099");
    }
    // The loop keeps renewing the registration.
    BOOST_CHECK(wait_until([&] { return backend->heartbeats >= 3; }, 2000ms));
    // Graceful stop unregisters; a second stop is a no-op.
    loop->stop();
    BOOST_CHECK(wait_until(
        [&] {
            return !backend->contains("self-a") && backend->unregisters >= 1;
        },
        2000ms));
    loop->stop();
}

BOOST_AUTO_TEST_CASE(LoopReportsAppearancesAndLosses) {
    auto backend = std::make_shared<FakeBackend>();
    Events events;
    auto loop = make_loop("self-a", backend, events);
    loop->start();
    backend->seed("peer-b", "127.0.0.1:21001");
    BOOST_CHECK(
        wait_until([&] { return events.saw_discovered("peer-b@"); }, 2000ms));
    backend->seed("peer-c", "127.0.0.1:21002");
    BOOST_CHECK(
        wait_until([&] { return events.saw_discovered("peer-c@"); }, 2000ms));
    // Re-seeding an already-known node does not re-fire the callback.
    backend->seed("peer-b", "127.0.0.1:21001");
    // Disappearance fires; a later re-appearance is discovered again.
    backend->remove("peer-b");
    BOOST_CHECK(wait_until([&] { return events.saw_lost("peer-b"); }, 2000ms));
    backend->seed("peer-b", "127.0.0.1:21001");
    BOOST_CHECK(
        wait_until([&] { return events.saw_discovered("peer-b@"); }, 2000ms));
    loop->stop();
}

BOOST_AUTO_TEST_CASE(LoopSurvivesBackendFailures) {
    auto backend = std::make_shared<FakeBackend>();
    backend->fail_register = true;
    backend->fail_heartbeat = true;
    backend->fail_scan = true;
    Events events;
    auto loop = make_loop("self-a", backend, events);
    loop->start();
    // Several failing rounds: no callbacks, loop still spinning.
    std::this_thread::sleep_for(30ms);
    BOOST_CHECK(events.discovered.empty());
    // Recovery: the same loop picks everything up again.
    backend->fail_register = false;
    backend->fail_heartbeat = false;
    backend->fail_scan = false;
    backend->seed("peer-b", "127.0.0.1:21001");
    BOOST_CHECK(
        wait_until([&] { return events.saw_discovered("peer-b@"); }, 3000ms));
    loop->stop();
    BOOST_CHECK(backend->unregisters >= 1);
}

BOOST_AUTO_TEST_CASE(StopSurvivesUnregisterFailure) {
    auto backend = std::make_shared<FakeBackend>();
    backend->fail_unregister = true;
    Events events;
    auto loop = make_loop("self-a", backend, events);
    loop->start();
    std::this_thread::sleep_for(10ms);
    // must not hang or throw despite the failing unregister
    const auto started_at = std::chrono::steady_clock::now();
    loop->stop();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_at)
                             .count();
    BOOST_CHECK_LT(stop_ms, 2000);
    backend->fail_unregister = false;
    backend->remove("self-a");  // the failed unregister left the record
}

// ---------------------------------------------------------------------------
// ClusterManager integration
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ManagerWiresDiscoveryIntoNodeTable) {
    ClusterConfig cfg;
    cfg.enabled = true;
    cfg.node_id = "mgr-a";
    cfg.listen_address = "127.0.0.1:21051";
    cfg.peers = {"127.0.0.1:21052"};
    cfg.heartbeat_interval_ms = 50;
    cfg.suspect_timeout_ms = 30000;
    cfg.offline_timeout_ms = 30000;
    ClusterManager mgr(cfg);

    auto backend = std::make_shared<FakeBackend>();
    backend->seed("dyn-b", "127.0.0.1:21053");
    // Announces the same dial target as the static peer list: no duplicate.
    backend->seed("dup", "127.0.0.1:21052");
    Events events;
    mgr.set_node_discovery(make_loop("mgr-a", backend, events));

    mgr.start();
    // Discovered address enters the node table exactly like a configured
    // peer (Connecting; the transport would dial it).
    BOOST_CHECK(wait_until(
        [&] {
            const auto* node = mgr.find_node("127.0.0.1:21053");
            return node && node->state == NodeState::Connecting;
        },
        2000ms));
    // The duplicate announcement did not fork the static peer's entry.
    std::size_t static_entries = 0;
    for (const auto& node : mgr.nodes()) {
        if (node.address == "127.0.0.1:21052") ++static_entries;
    }
    BOOST_CHECK_EQUAL(static_entries, 1u);
    BOOST_CHECK(mgr.find_node("dup") == nullptr);

    // Adopt dyn-b the way the transport would (handshake renames the
    // entry), then a vanished record degrades it to Suspect.
    mgr.on_handshake("127.0.0.1:21053", "dyn-b", 1);
    BOOST_CHECK(mgr.find_node("dyn-b")->state == NodeState::Online);
    backend->remove("dyn-b");
    BOOST_CHECK(wait_until(
        [&] {
            const auto* node = mgr.find_node("dyn-b");
            return node && node->state == NodeState::Suspect;
        },
        2000ms));
    // Repeated losses on a Suspect node are no-ops; so is an unknown id.
    mgr.on_discovery_lost("dyn-b");
    mgr.on_discovery_lost("never-seen");
    BOOST_CHECK(mgr.find_node("dyn-b")->state == NodeState::Suspect);

    // Stopping the manager unregisters this node via the discovery loop.
    mgr.stop();
    BOOST_CHECK(
        wait_until([&] { return !backend->contains("mgr-a"); }, 2000ms));
}

BOOST_AUTO_TEST_CASE(ManagerAcceptsDiscoveryAfterStart) {
    ClusterConfig cfg;
    cfg.enabled = true;
    cfg.node_id = "mgr-a";
    cfg.listen_address = "127.0.0.1:21061";
    cfg.heartbeat_interval_ms = 50;
    cfg.suspect_timeout_ms = 30000;
    cfg.offline_timeout_ms = 30000;
    ClusterManager mgr(cfg);
    mgr.start();
    // Attaching to a running manager begins discovering immediately.
    auto backend = std::make_shared<FakeBackend>();
    backend->seed("peer-b", "127.0.0.1:21062");
    Events events;
    mgr.set_node_discovery(make_loop("mgr-a", backend, events));
    BOOST_CHECK(wait_until(
        [&] {
            const auto* node = mgr.find_node("127.0.0.1:21062");
            return node && node->state == NodeState::Connecting;
        },
        2000ms));
    mgr.stop();
}

BOOST_AUTO_TEST_CASE(ParseDiscoveryConfigSection) {
    auto& cfg = shield::config::global_config();
    cfg.load_yaml_string(
        "cluster:\n"
        "  node_id: cfg-node\n"
        "  discovery:\n"
        "    type: redis\n"
        "    host: 10.0.0.9\n"
        "    port: 7100\n"
        "    password: sekret\n"
        "    db: 3\n"
        "    prefix: shield:test:nodes\n"
        "    ttl_seconds: 7\n"
        "    heartbeat_interval_ms: 1500\n"
        "    scan_interval_ms: 2500\n");
    const auto cc = parse_cluster_config();
    BOOST_CHECK_EQUAL(cc.discovery.type, "redis");
    BOOST_CHECK_EQUAL(cc.discovery.host, "10.0.0.9");
    BOOST_CHECK_EQUAL(cc.discovery.port, 7100);
    BOOST_CHECK_EQUAL(cc.discovery.password, "sekret");
    BOOST_CHECK_EQUAL(cc.discovery.db, 3);
    BOOST_CHECK_EQUAL(cc.discovery.prefix, "shield:test:nodes");
    BOOST_CHECK_EQUAL(cc.discovery.ttl_seconds, 7);
    BOOST_CHECK_EQUAL(cc.discovery.heartbeat_interval_ms, 1500);
    BOOST_CHECK_EQUAL(cc.discovery.scan_interval_ms, 2500);
    shield::config::reset_config();
}

BOOST_AUTO_TEST_CASE(ParseDiscoveryDefaultsWhenSectionAbsent) {
    auto& cfg = shield::config::global_config();
    cfg.load_yaml_string("cluster:\n  node_id: cfg-node\n");
    const auto cc = parse_cluster_config();
    BOOST_CHECK_EQUAL(cc.discovery.type, "");
    BOOST_CHECK_EQUAL(cc.discovery.host, "127.0.0.1");
    BOOST_CHECK_EQUAL(cc.discovery.port, 6379);
    BOOST_CHECK_EQUAL(cc.discovery.prefix, "shield:nodes");
    BOOST_CHECK_EQUAL(cc.discovery.ttl_seconds, 10);
    shield::config::reset_config();
}

BOOST_AUTO_TEST_CASE(MakeNodeDiscoveryDispatch) {
    DiscoveryConfig off;
    off.type = "";
    BOOST_CHECK(make_node_discovery(off, "n", "127.0.0.1:1") == nullptr);

    DiscoveryConfig unknown;
    unknown.type = "consul";
    BOOST_CHECK(make_node_discovery(unknown, "n", "127.0.0.1:1") == nullptr);

    // A redis-configured loop builds (lazy backend) and survives a
    // start/stop cycle against a closed port (register failure is logged,
    // never fatal).
#ifdef _WIN32
    const int closed_port = 1;  // loopback port 1 is effectively never open
#else
    const int closed_port = probe_free_port();
    BOOST_REQUIRE(closed_port > 0);
#endif
    DiscoveryConfig redis_cfg;
    redis_cfg.type = "redis";
    redis_cfg.port = closed_port;
    auto loop = make_node_discovery(redis_cfg, "n", "127.0.0.1:1");
    BOOST_REQUIRE(loop != nullptr);
    loop->start();
    std::this_thread::sleep_for(30ms);
    loop->stop();
}

// ---------------------------------------------------------------------------
// Real redis++ backend against a self-spawned redis-server (Linux/CI legs)
// ---------------------------------------------------------------------------

#ifndef _WIN32

BOOST_AUTO_TEST_CASE(RedisBackendLifecycle) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:ndtest:" + std::to_string(::getpid()) + ":lifecycle";
    auto a = make_redis_discovery_backend(redis_cfg(*srv, prefix, 1));
    auto b = make_redis_discovery_backend(redis_cfg(*srv, prefix, 1));

    a->register_node("node-a", "127.0.0.1:21041", 1);
    // A different node's scan sees the registration with its address.
    auto seen = b->scan("node-b");
    BOOST_REQUIRE_EQUAL(seen.size(), 1u);
    BOOST_CHECK_EQUAL(seen[0].node_id, "node-a");
    BOOST_CHECK_EQUAL(seen[0].address, "127.0.0.1:21041");
    // One's own id never comes back.
    BOOST_CHECK(a->scan("node-a").empty());

    // Heartbeat renewal: 1.2s after register with a 1s TTL the record is
    // still alive because the mid-point heartbeat refreshed it.
    std::this_thread::sleep_for(600ms);
    a->heartbeat("node-a", 1);
    std::this_thread::sleep_for(600ms);
    BOOST_CHECK_EQUAL(b->scan("node-b").size(), 1u);

    // Graceful unregister removes it.
    a->unregister_node("node-a");
    BOOST_CHECK(b->scan("node-b").empty());
}

BOOST_AUTO_TEST_CASE(RedisBackendTtlExpiresSilentNode) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:ndtest:" + std::to_string(::getpid()) + ":ttl";
    auto a = make_redis_discovery_backend(redis_cfg(*srv, prefix, 1));
    a->register_node("ghost", "127.0.0.1:21042", 1);
    // A crashed node (no heartbeats) expires out of the registry.
    std::this_thread::sleep_for(1500ms);
    auto b = make_redis_discovery_backend(redis_cfg(*srv, prefix, 1));
    BOOST_CHECK(b->scan("node-b").empty());
}

BOOST_AUTO_TEST_CASE(DiscoveryLoopEndToEnd) {
    const auto srv = spawn_redis_server();
    if (!srv) {
        BOOST_TEST_MESSAGE("redis-server unavailable; skipping");
        return;
    }
    const std::string prefix =
        "shield:ndtest:" + std::to_string(::getpid()) + ":e2e";
    Events events_a;
    auto loop_a = std::make_unique<NodeDiscovery>(
        "node-a", "127.0.0.1:21043",
        make_redis_discovery_backend(redis_cfg(*srv, prefix, 1)), 50, 50, 1);
    loop_a->set_callbacks(
        [&events_a](const DiscoveredNode& n) { events_a.on_discovered(n); },
        [&events_a](const std::string& id) { events_a.on_lost(id); });
    loop_a->start();

    // A second live node registers itself; A discovers it.
    auto loop_b = std::make_unique<NodeDiscovery>(
        "node-b", "127.0.0.1:21044",
        make_redis_discovery_backend(redis_cfg(*srv, prefix, 1)), 50, 50, 1);
    loop_b->start();
    BOOST_CHECK(wait_until(
        [&] { return events_a.saw_discovered("node-b@127.0.0.1:21044"); },
        5000ms));

    // B's graceful stop unregisters it; A reports the loss (not the TTL
    // wait — the record disappears within a scan interval).
    loop_b->stop();
    BOOST_CHECK(
        wait_until([&] { return events_a.saw_lost("node-b"); }, 5000ms));
    loop_a->stop();
}

BOOST_AUTO_TEST_CASE(RedisBackendClosedPortThrows) {
    const int port = probe_free_port();  // closed: nobody listens
    BOOST_REQUIRE(port > 0);
    RedisDiscoveryConfig cfg;
    cfg.port = port;
    cfg.prefix = "shield:ndtest:closed";
    auto backend = make_redis_discovery_backend(cfg);
    BOOST_CHECK_THROW(backend->register_node("n", "127.0.0.1:1", 5),
                      std::exception);
    BOOST_CHECK_THROW(backend->heartbeat("n", 5), std::exception);
    BOOST_CHECK_THROW(backend->scan("n"), std::exception);
    BOOST_CHECK_THROW(backend->unregister_node("n"), std::exception);
}

#endif  // !_WIN32
