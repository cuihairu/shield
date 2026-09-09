// Coverage cases for bootstrap.cpp error paths that do not require plugin
// binaries: invalid TCP endpoints, unresolved codec providers, the HTTP ops
// server failing to start, and the console-server teardown path.
#define BOOST_TEST_MODULE CovBootstrap2
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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

// An unparseable http.host makes the ops-server start() throw inside the
// guarded block; the error is logged and initialization still completes.
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

BOOST_AUTO_TEST_SUITE_END()
