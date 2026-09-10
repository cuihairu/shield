// Coverage tests for the M4 cluster data plane: the LuaServiceManager
// proxied-call primitives, transport envelope edge paths, and the cluster
// branches of the shield Lua API (send / call / call_timeout against a
// "node:service" target).
//
// The Lua API cases run without a network: the ClusterManager is pointed at
// a scripted remote-send function and a handshake-adopted "phantom" peer, so
// resolve_remote_target's pre-flight and the send/call envelopes are exercised
// deterministically. Only built in SHIELD_ENABLE_CLUSTER configurations
// (see tests/coverage/CMakeLists.txt).
#define BOOST_TEST_MODULE CovCluster
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/io/middleman.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <string>
#include <thread>
#include <vector>

#include "shield/caf_initializer.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_messages.hpp"
#include "shield/cluster/cluster_transport.hpp"
#include "shield/config/config.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;
using shield::cluster::ClusterConfig;
using shield::cluster::ClusterManager;
using shield::cluster::EnvelopeMsg;
using shield::cluster::EnvelopeReplyMsg;
using shield::cluster::HeartbeatMsg;
using shield::cluster::HelloAckMsg;
using shield::cluster::HelloMsg;
using shield::cluster::RoutesMsg;

namespace {

const std::string kTmpDir = "/tmp/shield_cov_cluster";

std::string write_script(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(kTmpDir);
    const std::string path = kTmpDir + "/" + name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

std::string opts_for(const std::string& name,
                     nlohmann::json extra = nlohmann::json::object()) {
    nlohmann::json opts = {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        opts[it.key()] = it.value();
    }
    return opts.dump();
}

// A manager whose phantom peer "node-b" is Online without any transport:
// resolve_remote_target's reachability pre-flight passes and remote sends
// land in the scripted function instead of the wire.
struct PhantomPeerManager {
    ClusterConfig config;
    std::unique_ptr<ClusterManager> manager;

    PhantomPeerManager() {
        config.enabled = true;
        config.node_id = "cov-a";
        config.listen_address = "127.0.0.1:0";
        config.peers = {"127.0.0.1:59999"};
        manager = std::make_unique<ClusterManager>(config);
        manager->start();
        manager->on_handshake("127.0.0.1:59999", "node-b", 7);
    }
    ~PhantomPeerManager() {
        manager->stop();
        shield::cluster::set_global_cluster_manager(nullptr);
    }
};

// Scripted remote send: records the last envelope, returns the scripted
// outcome, and optionally completes the call from a helper thread (the
// remote round-trip a transport would provide).
struct ScriptedSend {
    std::string node;
    std::string service_id;
    std::string method;
    std::string args_json;
    uint64_t session = 0;
    int32_t timeout = 0;
    bool outcome = true;
    std::string error;
    // When set, the invoke spawns a thread that completes the call after a
    // short delay; the thread is joined on destruction.
    LuaServiceManager* completer = nullptr;
    nlohmann::json reply = nlohmann::json::array({"pong"});
    std::vector<std::thread> workers;

    ~ScriptedSend() {
        for (auto& t : workers) t.join();
    }

    bool invoke(const std::string& target_node, const std::string& sid,
                const std::string& m, const std::string& args, uint64_t s,
                int32_t tmo, std::string* err) {
        node = target_node;
        service_id = sid;
        method = m;
        args_json = args;
        session = s;
        timeout = tmo;
        if (!outcome) {
            if (err) *err = error;
            return false;
        }
        if (completer && s != 0) {
            workers.emplace_back([this, s]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                completer->complete_call(s, true, reply);
            });
        }
        return true;
    }
};

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

bool run_script(sol::state& lua, const std::string& code) {
    auto result = lua.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error e = result;
        BOOST_TEST_MESSAGE("lua error: " << e.what());
        return false;
    }
    return true;
}

// The LuaServiceManager proxied-call primitive cases (begin/finish/abandon,
// dispatch_remote_call, call_with_session) live in test_cov_lua_service3.cpp:
// they are unconditional LuaServiceManager seams and must count toward the
// cluster-less coverage build too.

// Caller service whose handler issues cross-node shield.call.
const char* kRemoteCallerScript = R"lua(
local M = {}
local st = {}
function M.do_remote(ctx, target, method, payload)
  local ok, v = shield.call(target, method, payload)
  st.ok = ok
  st.v = tostring(v)
  st.code = type(v) == 'table' and (v.code or '') or ''
  return ok
end
function M.state(ctx) return st.ok, st.v, st.code end
return M
)lua";

// Local echo service that publishes an extra (alias) name from its spawn
// config — the same pattern as the messaging test service.
const char* kLocalAliasScript = R"lua(
local M = {}
function M.on_init(args)
    local config = (args and args.config) or {}
    if config.register_alias then
        shield.register(config.register_alias)
    end
    return true
end
function M.echo(ctx, v) return v end
return M
)lua";

}  // namespace

// ---------------------------------------------------------------------------
// Transport edge paths on a lonely node (no peers): envelope sends miss,
// unknown proxied sessions are dropped, stop is idempotent.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TransportEdgePaths) {
    ClusterConfig config;
    config.enabled = true;
    config.node_id = "cov-solo";
    config.listen_address = "127.0.0.1:0";
    caf::core::init_global_meta_objects();
    caf::io::middleman::init_global_meta_objects();
    shield::cluster::init_cluster_caf_types();
    caf::actor_system_config caf_config;
    caf_config.load<caf::io::middleman>();
    // The manager must outlive the transport actor (which holds a raw
    // pointer); heap-allocate it and release it after the system is gone.
    auto manager = std::make_unique<ClusterManager>(config);
    auto system = std::make_unique<caf::actor_system>(caf_config);

    auto transport = std::make_unique<shield::cluster::ClusterTransport>(
        *system, *manager, config);
    uint16_t bound = 0;
    std::string error;
    BOOST_CHECK(transport->start(&bound, error));
    BOOST_CHECK_GE(bound, 1);
    // A second start is a no-op reporting the same bound port.
    uint16_t bound_again = 0;
    BOOST_CHECK(transport->start(&bound_again, error));
    BOOST_CHECK_EQUAL(bound_again, bound);

    // No peers: every envelope send misses the routing table.
    std::string send_error;
    BOOST_CHECK(!transport->send_envelope("node-b", "sid", "echo", "[]", 0, 0,
                                          &send_error));
    BOOST_CHECK_EQUAL(send_error, "node_offline");

    // Unknown proxied sessions complete nowhere (honest drop).
    transport->complete_proxied_call(987654, true, "[]", "", "");

    transport->stop();
    // A second stop is a no-op, and sends keep failing after the stop.
    transport->stop();
    BOOST_CHECK(!transport->send_envelope("node-b", "sid", "echo", "[]", 0, 0,
                                          &send_error));
    // Teardown order mirrors the integration fixtures: the transport actor
    // dies before the system, and the system before the manager.
    transport.reset();
    system.reset();
    manager->stop();
    manager.reset();
}

// ---------------------------------------------------------------------------
// Transport start failure paths: a listen address that cannot be published
// (port already bound elsewhere), and peer addresses the actor cannot parse
// (skipped with a log, never a crash).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TransportStartFailurePaths) {
    caf::core::init_global_meta_objects();
    caf::io::middleman::init_global_meta_objects();
    shield::cluster::init_cluster_caf_types();
    caf::actor_system_config caf_config;
    caf_config.load<caf::io::middleman>();

    // Occupy a loopback port with a plain listener first.
    int blocker = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(blocker, 0);
    sockaddr_in baddr{};
    baddr.sin_family = AF_INET;
    baddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    baddr.sin_port = 0;
    BOOST_REQUIRE_EQUAL(
        ::bind(blocker, reinterpret_cast<sockaddr*>(&baddr), sizeof(baddr)), 0);
    BOOST_REQUIRE_EQUAL(::listen(blocker, 1), 0);
    socklen_t blen = sizeof(baddr);
    BOOST_REQUIRE_EQUAL(
        ::getsockname(blocker, reinterpret_cast<sockaddr*>(&baddr), &blen), 0);
    const uint16_t blocked_port = ntohs(baddr.sin_port);

    ClusterConfig blocked;
    blocked.enabled = true;
    blocked.node_id = "cov-blocked";
    blocked.listen_address = "127.0.0.1:" + std::to_string(blocked_port);
    auto manager = std::make_unique<ClusterManager>(blocked);
    auto system = std::make_unique<caf::actor_system>(caf_config);
    auto transport = std::make_unique<shield::cluster::ClusterTransport>(
        *system, *manager, blocked);
    std::string error;
    uint16_t bound = 0;
    BOOST_CHECK(!transport->start(&bound, error));
    BOOST_CHECK(error.find("cluster listen failed") != std::string::npos);
    ::close(blocker);
    transport.reset();
    system.reset();
    manager->stop();
    manager.reset();

    // Unparseable peer addresses are skipped at connect-tick time.
    ClusterConfig badpeers;
    badpeers.enabled = true;
    badpeers.node_id = "cov-badpeers";
    badpeers.listen_address = "127.0.0.1:0";
    badpeers.peers = {"no-colon-here", ":123", "host:"};
    auto manager2 = std::make_unique<ClusterManager>(badpeers);
    auto system2 = std::make_unique<caf::actor_system>(caf_config);
    auto transport2 = std::make_unique<shield::cluster::ClusterTransport>(
        *system2, *manager2, badpeers);
    uint16_t bound2 = 0;
    BOOST_CHECK(transport2->start(&bound2, error));
    // The first connect tick runs immediately after start; give the actor a
    // moment to chew through the invalid entries.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    transport2.reset();
    system2.reset();
    manager2->stop();
    manager2.reset();
}

// ---------------------------------------------------------------------------
// Malformed / unmatched inbound control messages, injected at the published
// transport actor from a foreign actor system: version mismatches, an ack
// that matches no dialed peer, heartbeats and route tables for unknown
// nodes, and a call envelope whose source has no connection (the reply
// drops). None of these may crash or adopt anything.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(TransportMalformedInjections) {
    caf::core::init_global_meta_objects();
    caf::io::middleman::init_global_meta_objects();
    shield::cluster::init_cluster_caf_types();
    caf::actor_system_config caf_config;
    caf_config.load<caf::io::middleman>();

    ClusterConfig config;
    config.enabled = true;
    config.node_id = "cov-target";
    config.listen_address = "127.0.0.1:0";
    auto manager = std::make_unique<ClusterManager>(config);
    auto system = std::make_unique<caf::actor_system>(caf_config);
    auto transport = std::make_unique<shield::cluster::ClusterTransport>(
        *system, *manager, config);
    uint16_t port = 0;
    std::string error;
    BOOST_CHECK(transport->start(&port, error));

    std::atomic<bool> send_dispatched{false};
    shield::cluster::EnvelopeBridges bridges;
    bridges.send_dispatch = [&](const std::string&, const std::string&,
                                const std::string&) {
        send_dispatched = true;
        return false;  // unknown service: the send is dropped with a warning
    };
    bridges.call_begin = [](int32_t) {
        return 0u;  // allocation refused: the reply goes to a source with no
                    // connection and drops
    };
    transport->set_envelope_bridges(std::move(bridges));

    // Foreign system: holds a proxy to the target's transport actor.
    caf::actor_system_config foreign_config;
    foreign_config.load<caf::io::middleman>();
    caf::actor_system foreign(foreign_config);
    auto proxy = foreign.middleman().remote_actor("127.0.0.1", port);
    BOOST_REQUIRE_MESSAGE(proxy, "could not reach the transport actor");

    // Handshake mismatches: wrong protocol version on hello and ack, and a
    // well-formed ack from a node that was never dialed.
    caf::anon_send(*proxy, HelloMsg{"intruder", 5, 999});
    caf::anon_send(*proxy, HelloAckMsg{"intruder", 5, 999});
    caf::anon_send(*proxy, HelloAckMsg{"intruder", 5,
                                       shield::cluster::kClusterProtoVersion});
    // Liveness and routes for a node that was never adopted: the manager
    // seams drop both silently.
    caf::anon_send(*proxy, HeartbeatMsg{"intruder", 5, 1});
    caf::anon_send(*proxy, RoutesMsg{"intruder", 5, {}});
    // Envelopes: fire-and-forget hits the (failing) send bridge; a call
    // envelope gets a reply that has nowhere to go.
    caf::anon_send(*proxy,
                   EnvelopeMsg{"intruder", "ghost", "echo", "[]", 0, 0});
    caf::anon_send(*proxy,
                   EnvelopeMsg{"intruder", "ghost", "echo", "[]", 42, 100});
    // An inbound reply: with no caller-side bridge session, the completion
    // is an honest no-op.
    caf::anon_send(*proxy, EnvelopeReplyMsg{777, true, "[]", "", ""});

    BOOST_CHECK(wait_until([&] { return send_dispatched.load(); },
                           std::chrono::milliseconds(5000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Nothing was adopted: the peer table is still empty.
    BOOST_CHECK(manager->find_node("intruder") == nullptr);

    transport.reset();
    system.reset();
    manager->stop();
    manager.reset();
}

// ---------------------------------------------------------------------------
// shield.cluster.* Lua bindings against a phantom cluster manager (no
// network): module-unavailable, reachability errors, route hits, node
// snapshot, and the node id / epoch accessors.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ClusterLuaApiPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    // No global manager: every accessor degrades honestly.
    BOOST_CHECK(
        run_script(lua,
                   "local v, err = shield.cluster.query('node-b', 'svc')\n"
                   "assert(v == nil)\n"
                   "assert(err.code == 'module_unavailable')\n"
                   "assert(#shield.cluster.nodes() == 0)\n"
                   "assert(shield.cluster.node_id() == nil)\n"
                   "assert(shield.cluster.node_epoch() == nil)"));

    PhantomPeerManager cluster;
    cluster.manager->register_route("node-b", "svc", "sid-1");
    shield::cluster::set_global_cluster_manager(cluster.manager.get());

    // Route hit.
    BOOST_CHECK(
        run_script(lua,
                   "local v, err = shield.cluster.query('node-b', 'svc')\n"
                   "assert(v == 'sid-1')\n"
                   "assert(err == nil)"));
    // Known node, unknown service.
    BOOST_CHECK(
        run_script(lua,
                   "local v, err = shield.cluster.query('node-b', 'missing')\n"
                   "assert(v == nil)\n"
                   "assert(err.code == 'service_not_found')"));
    // Unknown node: the reachability pre-flight answers first.
    BOOST_CHECK(
        run_script(lua,
                   "local v, err = shield.cluster.query('ghost', 'svc')\n"
                   "assert(v == nil)\n"
                   "assert(err.code == 'node_not_found')"));

    // Node snapshot carries the adopted phantom peer.
    BOOST_CHECK(run_script(lua,
                           "local nodes = shield.cluster.nodes()\n"
                           "assert(#nodes == 1)\n"
                           "assert(nodes[1].node_id == 'node-b')\n"
                           "assert(nodes[1].state == 'online')\n"
                           "assert(type(nodes[1].epoch) == 'string')"));
    // Self identity accessors.
    BOOST_CHECK(run_script(lua,
                           "assert(shield.cluster.node_id() == 'cov-a')\n"
                           "local epoch = shield.cluster.node_epoch()\n"
                           "assert(type(epoch) == 'string')\n"
                           "assert(#epoch > 0)"));
}

// ---------------------------------------------------------------------------
// ClusterManager unit gaps: the seamless send_remote error, route
// invalidation, the node-state serializer, and the string-peers variant of
// parse_cluster_config.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ManagerUnitGaps) {
    ClusterConfig config;
    config.enabled = true;
    config.node_id = "cov-units";
    config.listen_address = "127.0.0.1:0";
    ClusterManager manager(config);

    // No remote send function injected: the seam reports node_offline.
    std::string error;
    BOOST_CHECK(
        !manager.send_remote("node-b", "svc", "echo", "[]", 0, 0, &error));
    BOOST_CHECK_EQUAL(error, "node_offline");

    // Route invalidation: a registered route is queryable until cleared.
    manager.register_route("node-b", "svc", "sid-1");
    BOOST_CHECK_EQUAL(manager.query_remote("node-b", "svc"), "sid-1");
    manager.clear_routes("node-b");
    BOOST_CHECK(manager.query_remote("node-b", "svc").empty());

    // The node-state serializer covers every state.
    using shield::cluster::node_state_name;
    using shield::cluster::NodeState;
    BOOST_CHECK_EQUAL(node_state_name(NodeState::Connecting), "connecting");
    BOOST_CHECK_EQUAL(node_state_name(NodeState::Online), "online");
    BOOST_CHECK_EQUAL(node_state_name(NodeState::Suspect), "suspect");
    BOOST_CHECK_EQUAL(node_state_name(NodeState::Offline), "offline");
    BOOST_CHECK_EQUAL(node_state_name(NodeState::Removed), "removed");

    // parse_cluster_config: peers given as a comma/newline-separated string
    // instead of a YAML sequence.
    auto& cfg = shield::config::global_config();
    BOOST_CHECK(cfg.load_yaml_string(
        "cluster:\n"
        "  node_id: cov-parse\n"
        "  listen: 127.0.0.1:7001\n"
        "  heartbeat_interval_ms: 250\n"
        "  suspect_timeout_ms: 400\n"
        "  offline_timeout_ms: 800\n"
        "  peers: \"127.0.0.1:1, 127.0.0.1:2\\n127.0.0.1:3\"\n"));
    const auto cc = shield::cluster::parse_cluster_config();
    BOOST_CHECK(cc.enabled);
    BOOST_CHECK_EQUAL(cc.node_id, "cov-parse");
    BOOST_CHECK_EQUAL(cc.listen_address, "127.0.0.1:7001");
    BOOST_CHECK_EQUAL(cc.heartbeat_interval_ms, 250);
    BOOST_CHECK_EQUAL(cc.suspect_timeout_ms, 400);
    BOOST_CHECK_EQUAL(cc.offline_timeout_ms, 800);
    BOOST_REQUIRE_EQUAL(cc.peers.size(), 3u);
    BOOST_CHECK_EQUAL(cc.peers[0], "127.0.0.1:1");
    BOOST_CHECK_EQUAL(cc.peers[1], "127.0.0.1:2");
    BOOST_CHECK_EQUAL(cc.peers[2], "127.0.0.1:3");
    shield::config::reset_config();
}

// ---------------------------------------------------------------------------
// Inbound envelopes for unknown services: a fire-and-forget send is dropped,
// a call envelope is answered with a service_not_found reply to the source.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(EnvelopeUnknownServicePaths) {
    // Reserve two loopback ports up front: both nodes must configure each
    // other as peers, or the dialer-side identity adoption never happens on
    // one of the sides.
    auto free_port = []() {
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
    };
    const uint16_t port_a = free_port();
    const uint16_t port_b = free_port();

    struct Node {
        ClusterConfig config;
        std::unique_ptr<ClusterManager> manager;
        caf::actor_system_config caf_config;
        std::unique_ptr<caf::actor_system> system;
        std::unique_ptr<shield::cluster::ClusterTransport> transport;
        uint16_t port = 0;
    };
    auto make = [](const std::string& id, uint16_t listen, uint16_t peer_port) {
        Node n;
        n.config.enabled = true;
        n.config.node_id = id;
        n.config.listen_address = "127.0.0.1:" + std::to_string(listen);
        n.config.peers = {"127.0.0.1:" + std::to_string(peer_port)};
        // Short cadence so a dial that loses CAF's duplicate-connection
        // race heals within the wait budgets (same rationale as the
        // integration fixtures).
        n.config.heartbeat_interval_ms = 50;
        n.manager = std::make_unique<ClusterManager>(n.config);
        n.manager->start();
        n.caf_config.load<caf::io::middleman>();
        n.system = std::make_unique<caf::actor_system>(n.caf_config);
        n.transport = std::make_unique<shield::cluster::ClusterTransport>(
            *n.system, *n.manager, n.config);
        std::string error;
        BOOST_REQUIRE(n.transport->start(&n.port, error));
        return n;
    };

    Node a = make("cov-a", port_a, port_b);
    Node b = make("cov-b", port_b, port_a);
    // Wait for the handshake so reply routing has a peer entry on b.
    BOOST_CHECK(
        wait_until([&] { return a.manager->find_node("cov-b") != nullptr; },
                   std::chrono::milliseconds(5000)));

    std::atomic<bool> send_dispatched{false};
    std::atomic<bool> reply_landed{false};
    std::atomic<bool> reply_ok{false};
    std::string reply_code;
    shield::cluster::EnvelopeBridges a_bridges;
    a_bridges.reply_handler = [&](uint64_t, bool ok, const std::string&,
                                  const std::string& code, const std::string&) {
        reply_code = code;
        reply_ok = ok;
        reply_landed = true;  // release last: the test reads code after this
    };
    a.transport->set_envelope_bridges(std::move(a_bridges));

    shield::cluster::EnvelopeBridges b_bridges;
    b_bridges.send_dispatch = [&](const std::string&, const std::string&,
                                  const std::string&) {
        send_dispatched = true;
        return false;  // unknown service: the send is dropped
    };
    // Fully scripted callee side: allocate_mode 0 refuses the session
    // outright, 1 hands out fake sessions; dispatch_mode picks how a
    // dispatched call fails (0 = succeed, 1 = silent fail, 2 = fail+reason).
    std::atomic<int> allocate_mode{0};
    std::atomic<uint64_t> begin_seq{900};
    std::atomic<int> dispatch_mode{0};
    b_bridges.call_begin = [&](int32_t) {
        if (allocate_mode.load() == 0) return uint64_t{0};
        return begin_seq.fetch_add(1);
    };
    b_bridges.call_dispatch = [&](uint64_t, const std::string&,
                                  const std::string&, const std::string&,
                                  std::string* error) {
        switch (dispatch_mode.load()) {
            case 1:
                return false;  // immediate failure without a reason
            case 2:
                if (error) *error = "callee exploded";
                return false;
            default:
                return true;  // pretend the dispatch started; never completes
        }
    };
    b.transport->set_envelope_bridges(std::move(b_bridges));

    // Inject envelopes straight at b's published transport actor. Each round
    // waits for its own reply before the next injection, so exactly one
    // reply is in flight at a time and reply_code has no cross-talk.
    auto proxy = a.system->middleman().remote_actor("127.0.0.1", b.port);
    BOOST_REQUIRE_MESSAGE(proxy, "could not reach b's transport actor");
    auto inject = [&](uint64_t session) {
        reply_landed = false;
        caf::anon_send(*proxy, EnvelopeMsg{"cov-a", "ghost_svc", "echo", "[]",
                                           session, 1000});
        BOOST_CHECK(wait_until([&] { return reply_landed.load(); },
                               std::chrono::milliseconds(5000)));
    };
    caf::anon_send(*proxy,
                   EnvelopeMsg{"cov-a", "ghost_svc", "echo", "[]", 0, 0});
    BOOST_CHECK(wait_until([&] { return send_dispatched.load(); },
                           std::chrono::milliseconds(5000)));

    // Allocation refused: the fastest service_not_found reply.
    inject(77);
    BOOST_CHECK(!reply_ok.load());
    BOOST_CHECK_EQUAL(reply_code, "service_not_found");

    // Dispatch fails without a reason: the transport unregisters the
    // routing entry and answers with its default reason.
    allocate_mode = 1;
    dispatch_mode = 1;
    inject(78);
    BOOST_CHECK_EQUAL(reply_code, "service_not_found");

    // Dispatch fails carrying a reason: it wins over the default text.
    dispatch_mode = 2;
    inject(79);
    BOOST_CHECK_EQUAL(reply_code, "service_not_found");

    // Keep the nodes alive past a heartbeat tick: the liveness handler and
    // the piggybacked route tables must land (heartbeat + RoutesMsg paths).
    auto online = [](ClusterManager& m, const std::string& id) {
        auto* node = m.find_node(id);
        return node && node->state == shield::cluster::NodeState::Online;
    };
    BOOST_CHECK(wait_until(
        [&] {
            return online(*a.manager, "cov-b") && online(*b.manager, "cov-a");
        },
        std::chrono::milliseconds(5000)));

    a.transport->stop();
    b.transport->stop();
    a.manager->stop();
    b.manager->stop();
}

// ---------------------------------------------------------------------------
// Cluster branches of the Lua API: shield.send against remote targets
// (success, pre-flight failures, transport failures, self/local targets).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaRemoteSendPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    PhantomPeerManager cluster;

    ScriptedSend scripted;
    cluster.manager->set_remote_send_fn(
        [&scripted](const std::string& node, const std::string& sid,
                    const std::string& method, const std::string& args,
                    uint64_t session, int32_t timeout, std::string* err) {
            return scripted.invoke(node, sid, method, args, session, timeout,
                                   err);
        });
    // The route cache knows where "svc" lives on node-b.
    cluster.manager->register_route("node-b", "svc", "sid-1");
    shield::cluster::set_global_cluster_manager(cluster.manager.get());

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    // Remote send success: the envelope leaves with the resolved service id.
    scripted.outcome = true;
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('node-b:svc', 'echo', 1)\n"
                   "assert(ok == true)\n"
                   "assert(err == nil)"));
    BOOST_CHECK_EQUAL(scripted.node, "node-b");
    BOOST_CHECK_EQUAL(scripted.service_id, "sid-1");
    BOOST_CHECK_EQUAL(scripted.method, "echo");
    BOOST_CHECK_EQUAL(scripted.session, 0u);
    BOOST_CHECK_EQUAL(scripted.timeout, 0);

    // Unknown remote route: service_not_found, not retryable.
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('node-b:missing', 'echo')\n"
                   "assert(ok == false)\n"
                   "assert(err.code == 'service_not_found')\n"
                   "assert(err.retryable == false)"));

    // Unknown node: node_not_found from the reachability pre-flight.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield.send('ghost:svc', 'echo')\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'node_not_found')\n"
                           "assert(err.retryable == false)"));

    // Transport failure with an unmapped reason: transport_failed.
    scripted.outcome = false;
    scripted.error = "wire exploded";
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield.send('node-b:svc', 'echo')\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'transport_failed')\n"
                           "assert(err.retryable == false)"));

    // Transport failure carrying node_offline: retryable.
    scripted.error = "node_offline";
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield.send('node-b:svc', 'echo')\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'node_offline')\n"
                           "assert(err.retryable == true)"));

    // Self-qualified name: never reinterpreted as a remote target; the
    // qualified form is not a registrable local name, so it falls through
    // to a local miss without touching the scripted send.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield.send('cov-a:svc', 'echo')\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'service_not_found')"));
    const auto local = manager.spawn(
        write_script("cov_cluster_local.lua", kLocalAliasScript),
        opts_for("cov_local_impl",
                 {{"config", {{"register_alias", "local_svc"}}}}));
    BOOST_REQUIRE(local.success);
    // on_init (and therefore the alias registration) runs on the service
    // actor — wait until the name is reachable before sending to it.
    BOOST_REQUIRE(
        wait_until([&] { return !manager.query_service("local_svc").empty(); },
                   std::chrono::milliseconds(5000)));
    scripted.outcome = true;
    const std::string last_remote_node = scripted.node;
    BOOST_CHECK(run_script(
        lua,
        "local ok, err = shield.send('cov-a:local_svc', 'echo', 'hi')\n"
        "assert(ok == false)\n"
        "assert(err.code == 'service_not_found')"));
    BOOST_CHECK_EQUAL(scripted.node, last_remote_node);
    // The plain local alias still dispatches in-process.
    BOOST_CHECK(
        run_script(lua,
                   "local ok, err = shield.send('local_svc', 'echo', 'hi')\n"
                   "assert(ok == true)"));

    cluster.manager->set_remote_send_fn(nullptr);
}

// ---------------------------------------------------------------------------
// Main-thread synchronous remote call (shield._sync_call_timeout): success,
// initiate failure, and pre-flight failure mapping.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaRemoteSyncCallPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    PhantomPeerManager cluster;

    ScriptedSend scripted;
    scripted.completer = &manager;  // complete the call like a transport
    cluster.manager->set_remote_send_fn(
        [&scripted](const std::string& node, const std::string& sid,
                    const std::string& method, const std::string& args,
                    uint64_t session, int32_t timeout, std::string* err) {
            return scripted.invoke(node, sid, method, args, session, timeout,
                                   err);
        });
    cluster.manager->register_route("node-b", "svc", "sid-1");
    shield::cluster::set_global_cluster_manager(cluster.manager.get());

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::table,
                       sol::lib::string, sol::lib::os, sol::lib::math);
    register_full_shield_api(lua, &manager, &runtime);

    // Success: the scripted transport completes the session from its
    // thread; the main-thread call returns the values.
    scripted.outcome = true;
    BOOST_CHECK(run_script(lua,
                           "local ok, v = shield._sync_call_timeout(2000, "
                           "'node-b:svc', 'echo', 'x')\n"
                           "assert(ok == true)\n"
                           "assert(v == 'pong')"));
    BOOST_CHECK_EQUAL(scripted.session != 0, true);
    BOOST_CHECK_EQUAL(scripted.timeout, 2000);

    // Initiate failure: the transport error maps to a retryable code.
    scripted.outcome = false;
    scripted.error = "node_suspect";
    BOOST_CHECK(run_script(
        lua,
        "local ok, err = shield._sync_call_timeout(500, 'node-b:svc', 'echo')\n"
        "assert(ok == false)\n"
        "assert(err.code == 'node_suspect')\n"
        "assert(err.retryable == true)"));

    // Pre-flight failure (unknown route): fails without touching the wire.
    BOOST_CHECK(run_script(lua,
                           "local ok, err = shield._sync_call_timeout(500, "
                           "'node-b:missing', 'echo')\n"
                           "assert(ok == false)\n"
                           "assert(err.code == 'service_not_found')"));

    cluster.manager->set_remote_send_fn(nullptr);
}

// ---------------------------------------------------------------------------
// Coroutine-path remote call: the caller service yields inside shield.call
// and is resumed by the scripted transport's completion.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(LuaRemoteCoroutineCallPaths) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);
    LuaRuntime runtime;
    LuaServiceManager manager(runtime, system);
    PhantomPeerManager cluster;

    ScriptedSend scripted;
    scripted.completer = &manager;
    cluster.manager->set_remote_send_fn(
        [&scripted](const std::string& node, const std::string& sid,
                    const std::string& method, const std::string& args,
                    uint64_t session, int32_t timeout, std::string* err) {
            return scripted.invoke(node, sid, method, args, session, timeout,
                                   err);
        });
    cluster.manager->register_route("node-b", "svc", "sid-1");
    shield::cluster::set_global_cluster_manager(cluster.manager.get());

    const auto caller = manager.spawn(
        write_script("cov_cluster_caller.lua", kRemoteCallerScript),
        opts_for("cov_remote_caller"));
    BOOST_REQUIRE(caller.success);

    // Success path: the coroutine resumes with the remote values.
    scripted.outcome = true;
    auto fired =
        manager.call(caller.service_id, "do_remote",
                     nlohmann::json::array({"node-b:svc", "echo", "hi"}), 5000);
    BOOST_CHECK(fired.success);
    BOOST_CHECK(wait_until(
        [&] {
            auto state = manager.call(caller.service_id, "state",
                                      nlohmann::json::array(), 2000);
            return state.success && state.values.size() >= 2 &&
                   state.values[0].get<bool>();
        },
        std::chrono::milliseconds(5000)));

    // Send failure: the session is completed with the mapped error and the
    // caller resumes with ok=false + the retryable error table.
    scripted.outcome = false;
    scripted.error = "node_offline";
    auto second =
        manager.call(caller.service_id, "do_remote",
                     nlohmann::json::array({"node-b:svc", "echo", "hi"}), 5000);
    BOOST_CHECK(second.success);
    BOOST_CHECK(wait_until(
        [&] {
            auto state = manager.call(caller.service_id, "state",
                                      nlohmann::json::array(), 2000);
            return state.success && state.values.size() >= 3 &&
                   !state.values[0].get<bool>() &&
                   state.values[2].get<std::string>() == "node_offline";
        },
        std::chrono::milliseconds(5000)));

    // Pre-flight failure inside a coroutine: fails without a session.
    auto third = manager.call(
        caller.service_id, "do_remote",
        nlohmann::json::array({"node-b:missing", "echo", "x"}), 5000);
    BOOST_CHECK(third.success);
    BOOST_CHECK(wait_until(
        [&] {
            auto state = manager.call(caller.service_id, "state",
                                      nlohmann::json::array(), 2000);
            return state.success && state.values.size() >= 3 &&
                   !state.values[0].get<bool>() &&
                   state.values[2].get<std::string>() == "service_not_found";
        },
        std::chrono::milliseconds(5000)));

    cluster.manager->set_remote_send_fn(nullptr);
}
