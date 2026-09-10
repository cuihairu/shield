#define BOOST_TEST_MODULE CovShield
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifdef SHIELD_ENABLE_CLUSTER
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/io/middleman.hpp>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "shield/cluster/cluster_messages.hpp"
#include "shield/cluster/cluster_transport.hpp"
#endif

#include "shield/shield.hpp"

namespace {

namespace fs = std::filesystem;

fs::path write_temp(const std::string& name, const std::string& content) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream(p) << content;
    return p;
}

fs::path minimal_config() {
    fs::path script =
        write_temp("shield_cov_shield_echo.lua", "local M = {}\nreturn M\n");
    return write_temp("shield_cov_shield_app.yaml",
                      "app:\n"
                      "  name: cov\n"
                      "actors:\n"
                      "  - name: main\n"
                      "    script: " +
                          script.string() + "\n");
}

int run_args(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("shield"));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    return shield::run(static_cast<int>(argv.size()), argv.data());
}

#ifdef SHIELD_ENABLE_CLUSTER
// Reserve a free loopback port (bind + read the port + close). Used to pin
// cluster listen addresses in YAML configs without cross-test collisions.
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

// The glue service: echo for successful proxied completions, boom for the
// failure branch of the proxied-call hook.
const char* kGlueScript = R"lua(
local M = {}
function M.echo(ctx, v) return v end
function M.boom(ctx) error("glue exploded") end
return M
)lua";
#endif

}  // namespace

BOOST_AUTO_TEST_SUITE(ShieldRunTests)

BOOST_AUTO_TEST_CASE(HelpAndVersion) {
    BOOST_CHECK_EQUAL(run_args({"--help"}), 0);
    BOOST_CHECK_EQUAL(run_args({"-h"}), 0);
    BOOST_CHECK_EQUAL(run_args({"--version"}), 0);
    BOOST_CHECK_EQUAL(run_args({"-v"}), 0);
}

BOOST_AUTO_TEST_CASE(CliParseErrors) {
    // Unknown argument.
    BOOST_CHECK_EQUAL(run_args({"--frobnicate"}), 1);
    // Missing values.
    BOOST_CHECK_EQUAL(run_args({"--config"}), 1);
    BOOST_CHECK_EQUAL(run_args({"-c"}), 1);
    BOOST_CHECK_EQUAL(run_args({"--log-level"}), 1);
    BOOST_CHECK_EQUAL(run_args({"--workers"}), 1);
    BOOST_CHECK_EQUAL(run_args({"--node-id"}), 1);
    // Invalid worker counts.
    BOOST_CHECK_EQUAL(run_args({"--workers", "abc"}), 1);
    BOOST_CHECK_EQUAL(run_args({"--workers", "-3"}), 1);
    // --node-id parses but requires the cluster build.
    BOOST_CHECK_EQUAL(run_args({"--node-id", "cov-node"}), 1);
}

BOOST_AUTO_TEST_CASE(CheckConfigSucceeds) {
    fs::path cfg = minimal_config();
    BOOST_CHECK_EQUAL(run_args({"--config", cfg.string(), "--log-level",
                                "debug", "--workers", "2", "--check-config"}),
                      0);
    // Repeated -c flags override the default config list.
    BOOST_CHECK_EQUAL(
        run_args({"-c", cfg.string(), "-c", cfg.string(), "--check-config"}),
        0);
}

BOOST_AUTO_TEST_CASE(RunUntilStopSignal) {
    fs::path cfg = minimal_config();
    std::thread stopper([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        // In-process stop request instead of raise(SIGINT): the Windows
        // console handler only reacts to GenerateConsoleCtrlEvent, so a
        // raised SIGINT would never interrupt wait_for_stop() there.
        shield::request_stop();
    });
    int rc = run_args({"--config", cfg.string()});
    stopper.join();
    BOOST_CHECK_EQUAL(rc, 0);
}

BOOST_AUTO_TEST_CASE(FatalErrorReturnsTwo) {
    // http.port is parsed with std::stoi outside the guarded try block inside
    // bootstrap::initialize, so a non-numeric port propagates to shield::run's
    // top-level catch.
    fs::path script =
        write_temp("shield_cov_shield_echo2.lua", "local M = {}\nreturn M\n");
    fs::path cfg = write_temp("shield_cov_shield_bad_http.yaml",
                              "app:\n"
                              "  name: cov\n"
                              "http:\n"
                              "  enabled: true\n"
                              "  host: 127.0.0.1\n"
                              "  port: not_a_number\n"
                              "actors:\n"
                              "  - name: main\n"
                              "    script: " +
                                  script.string() + "\n");
    BOOST_CHECK_EQUAL(run_args({"--config", cfg.string()}), 2);
}

BOOST_AUTO_TEST_CASE(MissingConfigFailsInitialization) {
    BOOST_CHECK_EQUAL(
        run_args({"--config", "/tmp/shield_cov_does_not_exist.yaml"}), 1);
}

#ifdef SHIELD_ENABLE_CLUSTER
// A cluster-enabled run walks the M4 data-plane glue in bootstrap: the
// remote-send seam, the envelope bridges, and the proxied-call hook are all
// installed at startup, and teardown stops the transport before releasing
// the service manager. One node, one unreachable peer — no traffic needed.
BOOST_AUTO_TEST_CASE(ClusterEnabledRunStartsAndStops) {
    fs::path script = write_temp("shield_cov_shield_cluster_echo.lua",
                                 "local M = {}\nreturn M\n");
    fs::path cfg = write_temp("shield_cov_shield_cluster.yaml",
                              "app:\n"
                              "  name: cov-cluster\n"
                              "cluster:\n"
                              "  node_id: cov-node\n"
                              "  listen: 127.0.0.1:58231\n"
                              "  peers:\n"
                              "    - 127.0.0.1:59998\n"
                              "actors:\n"
                              "  - name: main\n"
                              "    script: " +
                                  script.string() + "\n");
    std::thread stopper([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        shield::request_stop();
    });
    int rc = run_args({"--config", cfg.string(), "--node-id", "cov-node"});
    stopper.join();
    BOOST_CHECK_EQUAL(rc, 0);
}

// An empty cluster.node_id (the key present, the value empty) is rejected at
// initialization: the node refuses to run half-configured.
BOOST_AUTO_TEST_CASE(ClusterConfigRequiresNodeId) {
    fs::path script =
        write_temp("shield_cov_glue_main.lua", "local M = {}\nreturn M\n");
    fs::path cfg = write_temp("shield_cov_cluster_nonode.yaml",
                              "app:\n"
                              "  name: cov-cluster-nonode\n"
                              "cluster:\n"
                              "  node_id: \"\"\n"
                              "  listen: 127.0.0.1:0\n"
                              "actors:\n"
                              "  - name: main\n"
                              "    script: " +
                                  script.string() + "\n");
    BOOST_CHECK_EQUAL(run_args({"--config", cfg.string()}), 1);
}

// A cluster listen address that cannot be bound (already occupied) is fatal
// at initialization: silently running standalone would betray the node's
// reported health.
BOOST_AUTO_TEST_CASE(ClusterTransportListenConflict) {
#ifndef _WIN32
    const uint16_t occupied = free_port();
    int blocker = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_GE(blocker, 0);
    sockaddr_in baddr{};
    baddr.sin_family = AF_INET;
    baddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    baddr.sin_port = htons(occupied);
    BOOST_REQUIRE_EQUAL(
        ::bind(blocker, reinterpret_cast<sockaddr*>(&baddr), sizeof(baddr)), 0);
    BOOST_REQUIRE_EQUAL(::listen(blocker, 1), 0);

    fs::path script =
        write_temp("shield_cov_glue_main2.lua", "local M = {}\nreturn M\n");
    fs::path cfg = write_temp("shield_cov_cluster_conflict.yaml",
                              "app:\n"
                              "  name: cov-cluster-conflict\n"
                              "cluster:\n"
                              "  node_id: cov-conflict\n"
                              "  listen: 127.0.0.1:" +
                                  std::to_string(occupied) +
                                  "\n"
                                  "actors:\n"
                                  "  - name: main\n"
                                  "    script: " +
                                  script.string() + "\n");
    BOOST_CHECK_EQUAL(run_args({"--config", cfg.string()}), 1);
    ::close(blocker);
#endif
}

// A full cluster-enabled run with the M4 data-plane glue installed: inbound
// envelopes injected at the published transport actor walk the send/call
// dispatch bridges, the proxied-call hook (success and failure), and the
// caller-side reply handler.
BOOST_AUTO_TEST_CASE(ClusterGlueHandlesInboundEnvelopes) {
    fs::path script = write_temp("shield_cov_glue_svc.lua", kGlueScript);
    const uint16_t listen_port = free_port();
    fs::path cfg = write_temp("shield_cov_cluster_glue.yaml",
                              "app:\n"
                              "  name: cov-cluster-glue\n"
                              "cluster:\n"
                              "  node_id: cov-glue\n"
                              "  listen: 127.0.0.1:" +
                                  std::to_string(listen_port) +
                                  "\n"
                                  "actors:\n"
                                  "  - name: glue_svc\n"
                                  "    script: " +
                                  script.string() + "\n");

    // Cluster wire types must be in CAF's global table before any
    // actor_system (ours below, or the run's) is constructed.
    shield::cluster::init_cluster_caf_types();

    int rc = 1;
    std::thread runner([&]() { rc = run_args({"--config", cfg.string()}); });

    // Bootstrap + service spawn take a moment on loaded CI machines.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    {
        caf::actor_system_config caf_config;
        caf_config.load<caf::io::middleman>();
        caf::actor_system system(caf_config);
        // The published transport actor may need a moment to accept BASP
        // connections after publish() returned. This CAF build's remote_actor
        // has no timeout overload, but on loopback a refused connect fails
        // immediately, so the retry loop itself bounds the wait.
        caf::actor proxy;
        for (int attempt = 0; attempt < 25 && !proxy; ++attempt) {
            if (auto found =
                    system.middleman().remote_actor("127.0.0.1", listen_port)) {
                proxy = *found;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        BOOST_REQUIRE_MESSAGE(proxy, "run's cluster transport did not come up");
        if (proxy) {
            // Fire-and-forget send → send_dispatch bridge.
            caf::anon_send(
                proxy, shield::cluster::EnvelopeMsg{"ext", "glue_svc", "echo",
                                                    "[\"hi\"]", 0, 0});
            // Call envelopes → call_begin/call_dispatch → proxied sessions:
            // one completes successfully (hook ok-branch; the reply is
            // addressed to "ext", which has no connection, and drops), one
            // fails inside the handler (hook error-branch).
            caf::anon_send(
                proxy, shield::cluster::EnvelopeMsg{"ext", "glue_svc", "echo",
                                                    "[\"yo\"]", 7, 2000});
            caf::anon_send(
                proxy, shield::cluster::EnvelopeMsg{"ext", "glue_svc", "boom",
                                                    "[]", 8, 2000});
            // Inbound replies → caller-side reply_handler bridge: one valid
            // payload, one error. Remote sessions (99/98) never collide with
            // the node's own proxied session ids.
            caf::anon_send(proxy, shield::cluster::EnvelopeReplyMsg{
                                      99, true, "[1,2]", "", ""});
            caf::anon_send(proxy,
                           shield::cluster::EnvelopeReplyMsg{
                               98, false, "", "node_offline", "peer gone"});
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    shield::request_stop();
    runner.join();
    BOOST_CHECK_EQUAL(rc, 0);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
