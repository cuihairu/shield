// Coverage cases for bootstrap.cpp error paths that do not require plugin
// binaries: invalid TCP endpoints, unresolved codec providers, the HTTP ops
// server failing to start, and the console-server teardown path.
#define BOOST_TEST_MODULE CovBootstrap2
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "shield/bootstrap/bootstrap.hpp"

namespace {

namespace fs = std::filesystem;

int g_seq = 0;

fs::path write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p);
    f << content;
    return p;
}

fs::path write_config(const std::string& content) {
    ++g_seq;
    return write_file(
        fs::temp_directory_path() /
            ("shield_cov_boot2_" + std::to_string(g_seq) + ".yaml"),
        content);
}

fs::path lua_script(const std::string& name) {
    ++g_seq;
    return write_file(
        fs::temp_directory_path() /
            ("shield_cov_boot2_" + std::to_string(g_seq) + ".lua"),
        "local M = {}\nreturn M\n");
}

// Ensure the global runtime is down even when an initialize() call behaves
// differently than the test expects, so later cases stay independent.
void force_shutdown() {
    if (shield::bootstrap::is_initialized()) {
        shield::bootstrap::shutdown();
    }
}

struct ShutdownGuard {
    ~ShutdownGuard() { force_shutdown(); }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(Bootstrap2Tests)

// parse_endpoint's failure branches are unreachable through the validated
// config path (config validation is stricter), so no endpoint test here.

// A protocol body referencing a codec provider that no plugin provides makes
// initialize() fail with the provider-not-configured error.
BOOST_AUTO_TEST_CASE(UnresolvedCodecProviderFailsInitialize) {
    ShutdownGuard guard;
    const fs::path script = lua_script("no_provider.lua");
    const fs::path cfg = write_config(
        "app:\n  name: noprov\n"
        "actors:\n  - name: a\n    script: " +
        script.string() +
        "\n    network:\n      tcp: \"127.0.0.1:18123\"\n      protocol:\n"
        "        name: cov\n        body:\n          codec: msgpack\n"
        "          provider: no.such.provider\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
}

// An unparseable http.host makes the ops-server start() report failure via
// its return value; the error is logged and initialization still completes.
BOOST_AUTO_TEST_CASE(HttpOpsBadHostIsNonFatal) {
    ShutdownGuard guard;
    const fs::path script = lua_script("ops_host.lua");
    const fs::path cfg = write_config(
        "app:\n  name: opshost\n"
        "http:\n  enabled: true\n  host: not-an-ip-address\n  port: 18099\n"
        "actors:\n  - name: a\n    script: " +
        script.string() + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());
    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
}

// console.enabled starts the console server; shutdown must stop it and reset
// the dispatcher (console teardown branch).
#ifndef _WIN32  // unix-domain console socket: POSIX only
BOOST_AUTO_TEST_CASE(ConsoleServerStartsAndStops) {
    ShutdownGuard guard;
    const fs::path script = lua_script("console_on.lua");
    const fs::path sock =
        fs::temp_directory_path() /
        ("shield_cov_boot2_" + std::to_string(::getpid()) + ".sock");
    std::error_code ec;
    fs::remove(sock, ec);
    const fs::path cfg = write_config(
        "app:\n  name: cons\n"
        "console:\n  enabled: true\n  socket_path: " +
        sock.string() +
        "\n"
        "actors:\n  - name: a\n    script: " +
        script.string() + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());
    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    std::error_code ec2;
    fs::remove(sock, ec2);
}
#endif  // !_WIN32

// An actor script that resolves to nothing (not absolute, not under the
// lua.script_path) falls through to the raw name and fails the spawn.
BOOST_AUTO_TEST_CASE(InitializeFailsOnMissingActorScript) {
    ShutdownGuard guard;
    fs::path cfg = write_config(
        "app:\n  name: no-script\n"
        "actors:\n  - name: a\n    script: definitely_missing_cov.lua\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
}

// A relative actor script resolves against the config file's directory (the
// source_dir arm of the resolver chain), an explicit RuntimeConfig node_id
// publishes through the cluster config, and every listener option guard
// takes its configured arm: the non-local host warns and binds all
// interfaces, and the connection/frame/queue/idle limits all install.
BOOST_AUTO_TEST_CASE(ListenerOptionsNodeIdAndRelativeScript) {
    ShutdownGuard guard;
    // The script sits next to the config (both land in the temp directory)
    // so its bare name resolves through source_dir, not the cwd.
    ++g_seq;
    const fs::path script =
        write_file(fs::temp_directory_path() /
                       ("shield_cov_boot2_" + std::to_string(g_seq) + ".lua"),
                   "local M = {}\nreturn M\n");
    const fs::path cfg = write_config(
        "app:\n  name: opts\n"
        "net:\n  threads: 2\n"
        "actors:\n  - name: opts_a\n    script: " +
        script.filename().string() +
        "\n    network:\n"
        "      tcp: \"203.0.113.7:18451\"\n"
        "      protocol:\n        body:\n          codec: json\n"
        "      max_connections: 5\n"
        "      max_connections_per_ip: 2\n"
        "      max_frame_size: 65536\n"
        "      max_session_send_queue: 100\n"
        "      read_idle_timeout: 30000\n"
        "      rate_limit_per_second: 100\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    rc.node_id = "cov-opts-node";
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());
    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
}

// instances > 1 spawns suffixed service names (the multi-instance naming
// arm); network actors are excluded from this by config validation, so the
// actor here has no listener.
BOOST_AUTO_TEST_CASE(MultiInstanceActorSpawnSuffixedNames) {
    ShutdownGuard guard;
    const fs::path script = lua_script("multi.lua");
    const fs::path cfg = write_config(
        "app:\n  name: multi\n"
        "actors:\n  - name: m\n    instances: 2\n    script: " +
        script.string() + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());
    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
}

// net.threads = 0 is the legacy single-threaded io loop arm.
BOOST_AUTO_TEST_CASE(LegacyZeroNetThreadsStillServes) {
    ShutdownGuard guard;
    const fs::path script = lua_script("legacy_net.lua");
    const fs::path cfg = write_config(
        "app:\n  name: legacy\n"
        "net:\n  threads: 0\n"
        "actors:\n  - name: a\n    script: " +
        script.string() +
        "\n    network:\n      tcp: \"127.0.0.1:18452\"\n"
        "      protocol:\n        body:\n          codec: json\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());
    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
}

// A relative actor script that is neither absolute nor next to the config
// resolves against the process working directory (the exists() arm of the
// resolver chain shared by bootstrap and lua_service).
BOOST_AUTO_TEST_CASE(RelativeScriptResolvesAgainstCwd) {
    ShutdownGuard guard;
    ++g_seq;
    const fs::path script =
        fs::current_path() /
        ("shield_cov_boot2_" + std::to_string(g_seq) + "_cwd.lua");
    write_file(script, "local M = {}\nreturn M\n");
    const fs::path cfg = write_config(
        "app:\n  name: cwd\n"
        "actors:\n  - name: a\n    script: " +
        script.filename().string() + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());
    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    std::error_code ec;
    fs::remove(script, ec);
}

// A console client drives the line dispatcher wired in initialize(): connect
// to the unix socket, send a command line, and read the JSON reply. The tcp
// actor keeps an io thread alive for the console server (a console-only
// config starts no net threads).
#ifndef _WIN32  // unix-domain console socket: POSIX only
BOOST_AUTO_TEST_CASE(ConsoleLineDispatchesCommand) {
    ShutdownGuard guard;
    const fs::path script = lua_script("console_line.lua");
    const fs::path sock =
        fs::temp_directory_path() /
        ("shield_cov_boot2_" + std::to_string(::getpid()) + "_line.sock");
    std::error_code ec;
    fs::remove(sock, ec);
    const fs::path cfg = write_config(
        "app:\n  name: consline\n"
        "console:\n  enabled: true\n  socket_path: " +
        sock.string() + "\n" +
        "actors:\n  - name: a\n    script: " + script.string() +
        "\n    network:\n      tcp: \"127.0.0.1:18461\"\n"
        "      protocol:\n        body:\n          codec: json\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));

    namespace local = boost::asio::local;
    boost::asio::io_context io;
    local::stream_protocol::socket client(io);
    boost::system::error_code connect_ec;
    client.connect(local::stream_protocol::endpoint(sock.string()), connect_ec);
    BOOST_REQUIRE(!connect_ec);
    boost::asio::write(client, boost::asio::buffer(std::string("help\n")));
    std::vector<char> reply(512);
    boost::system::error_code read_ec;
    std::size_t n = 0;
    client.async_read_some(
        boost::asio::buffer(reply),
        [&](const boost::system::error_code& e, std::size_t got) {
            read_ec = e;
            n = got;
        });
    io.run_for(std::chrono::seconds(3));
    BOOST_CHECK(!read_ec);
    BOOST_CHECK_GT(n, 0u);
    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    std::error_code ec2;
    fs::remove(sock, ec2);
}
#endif  // !_WIN32

BOOST_AUTO_TEST_SUITE_END()
