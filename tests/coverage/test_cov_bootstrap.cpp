#define BOOST_TEST_MODULE CovBootstrap
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "shield/bootstrap/bootstrap.hpp"
#include "shield/config/config.hpp"

namespace {

namespace fs = std::filesystem;

int g_config_seq = 0;

fs::path write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p);
    f << content;
    return p;
}

fs::path write_config(const std::string& content) {
    ++g_config_seq;
    return write_file(
        fs::temp_directory_path() /
            ("shield_cov_boot_" + std::to_string(g_config_seq) + ".yaml"),
        content);
}

fs::path echo_script(const std::string& name) {
    return write_file(fs::temp_directory_path() / name,
                      "local M = {}\nreturn M\n");
}

uint16_t free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acc(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    return acc.local_endpoint().port();
}

fs::path base_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Ensure the global runtime is down even when an initialize() call behaves
// differently than the test expects, so later cases stay independent.
void force_shutdown() {
    if (shield::bootstrap::is_initialized()) {
        shield::bootstrap::shutdown();
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(BootstrapTests)

BOOST_AUTO_TEST_CASE(InitializeFailsOnMissingConfigFile) {
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {"/tmp/shield_cov_boot_definitely_missing.yaml"};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
}

BOOST_AUTO_TEST_CASE(InitializeFailsOnInvalidConfig) {
    // Missing app.name fails runtime validation.
    fs::path cfg = write_config("log:\n  level: info\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
}

BOOST_AUTO_TEST_CASE(InitializeFailsOnPluginCatalogError) {
    // Two packages with the same manifest id make PluginHost::catalog fail,
    // which aborts initialization at plugin startup.
    fs::path plugins = base_dir("shield_cov_boot_dup_plugins");
    const char* manifest =
        "schema_version: 1\n"
        "id: dup.pkg\n"
        "name: Dup\n"
        "version: 1.0.0\n"
        "kind: test\n"
        "entry: shield_plugin_get_v1\n"
        "library:\n"
        "  linux: bin/lib.so\n"
        "  macos: bin/lib.dylib\n"
        "  windows: bin/lib.dll\n"
        "provides:\n"
        "  - interface: dup.iface\n"
        "requires: []\n"
        "config_schema:\n"
        "  type: object\n";
    for (const char* dir : {"dup_a", "dup_b"}) {
        fs::create_directories(plugins / dir);
        write_file(plugins / dir / "manifest.yaml", manifest);
    }

    fs::path script = echo_script("shield_cov_boot_dupactor.lua");
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "plugins:\n  directory: " +
        plugins.string() +
        "\n"
        "actors:\n"
        "  - name: main\n"
        "    script: " +
        script.string() + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
    fs::remove_all(plugins);
}

BOOST_AUTO_TEST_CASE(InitializeFailsOnRequiredActorSpawn) {
    fs::path bad =
        write_file(fs::temp_directory_path() / "shield_cov_boot_bad.lua",
                   "error('load time boom')\n");
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "actors:\n"
        "  - name: doomed\n"
        "    script: " +
        bad.string() + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
}

BOOST_AUTO_TEST_CASE(InitializeWithOptionalActorFailureAndDoubleInit) {
    fs::path bad =
        write_file(fs::temp_directory_path() / "shield_cov_boot_opt_bad.lua",
                   "error('optional boom')\n");
    fs::path good = echo_script("shield_cov_boot_opt_good.lua");
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "actors:\n"
        "  - name: flaky\n"
        "    required: false\n"
        "    script: " +
        bad.string() +
        "\n"
        "  - name: solid\n"
        "    script: " +
        good.string() + "\n");

    // config_file fallback (config_files empty) covers the legacy field.
    shield::bootstrap::RuntimeConfig rc;
    rc.config_file = cfg.string();
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());

    // Second initialize while running is a no-op returning true.
    BOOST_CHECK(shield::bootstrap::initialize(rc));

    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());

    // Shutdown when already shut down is a no-op.
    shield::bootstrap::shutdown();
}

BOOST_AUTO_TEST_CASE(InitializeLegacySingleNetThread) {
    fs::path script = echo_script("shield_cov_boot_legacy.lua");
    uint16_t port = free_port();
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "actors:\n"
        "  - name: gw\n"
        "    script: " +
        script.string() +
        "\n    network:\n      tcp: 127.0.0.1:" + std::to_string(port) + "\n");

    // An empty config_files entry is skipped; the real file is loaded.
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {"", cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));

    shield::bootstrap::shutdown();
}

BOOST_AUTO_TEST_CASE(CodecProviderMissingFails) {
    fs::path script = echo_script("shield_cov_boot_nocodec.lua");
    uint16_t port = free_port();
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "actors:\n"
        "  - name: gw\n"
        "    script: " +
        script.string() +
        "\n    network:\n      tcp: 127.0.0.1:" + std::to_string(port) +
        "\n      protocol:\n"
        "        name: cov\n"
        "        body:\n"
        "          codec: json\n"
        "          provider: nope.codec\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
}

BOOST_AUTO_TEST_CASE(NonBuiltinCodecWithoutProviderFailsProbe) {
    fs::path script = echo_script("shield_cov_boot_probe.lua");
    uint16_t port = free_port();
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "actors:\n"
        "  - name: gw\n"
        "    script: " +
        script.string() +
        "\n    network:\n      tcp: 127.0.0.1:" + std::to_string(port) +
        "\n      protocol:\n"
        "        name: cov\n"
        "        body:\n          codec: msgpack\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
}

BOOST_AUTO_TEST_CASE(DuplicateListenerPortFails) {
    fs::path script = echo_script("shield_cov_boot_dup.lua");
    uint16_t port = free_port();
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "actors:\n"
        "  - name: first\n"
        "    script: " +
        script.string() +
        "\n    network:\n      tcp: 127.0.0.1:" + std::to_string(port) +
        "\n"
        "  - name: second\n"
        "    script: " +
        script.string() +
        "\n    network:\n      tcp: 127.0.0.1:" + std::to_string(port) + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    // The second listener cannot bind (port in use) and the failure unwinds
    // the first listener via cleanup_failed_initialize.
    BOOST_CHECK(!shield::bootstrap::initialize(rc));
    BOOST_CHECK(!shield::bootstrap::is_initialized());
    force_shutdown();
}

BOOST_AUTO_TEST_CASE(ConsoleSocketFailureIsNonFatal) {
    fs::path script = echo_script("shield_cov_boot_console.lua");
    fs::path cfg = write_config(
        "app:\n  name: cov\n"
        "console:\n"
        "  enabled: true\n"
        "  socket_path: /nonexistent-shield-cov-dir/console.sock\n"
        "actors:\n"
        "  - name: main\n"
        "    script: " +
        script.string() + "\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    // The console failure is caught and logged; initialization succeeds.
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    shield::bootstrap::shutdown();
}

BOOST_AUTO_TEST_CASE(ScriptPathFloatTypoFallsBackToBareScriptName) {
    // Validation resolves the script through the raw YAML scalar ("1.10"),
    // while the bootstrap resolver reads the flattened double from config
    // storage ("1.100000"). With the script present only under "1.10", the
    // resolver falls back to the bare script name.
    fs::path dir = fs::path("1.10");
    fs::create_directories(dir);
    write_file(dir / "ghost.lua", "local M = {}\nreturn M\n");

    fs::path work = base_dir("shield_cov_boot_floatcfg");
    fs::path cfg = write_file(work / "app.yaml",
                              "app:\n  name: cov\n"
                              "lua:\n  script_path: 1.10\n"
                              "actors:\n"
                              "  - name: ghost\n"
                              "    required: false\n"
                              "    script: ghost.lua\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    shield::bootstrap::shutdown();
    fs::remove_all(dir);
    fs::remove_all(work);
}

BOOST_AUTO_TEST_CASE(XmldefCatalogRemovedAfterProbeFailsPerConnectionBuild) {
    // The startup probe loads the xmldef catalog successfully; removing the
    // catalog before a client connects makes the per-connection pipeline
    // factory log a build failure instead.
    fs::path work = base_dir("shield_cov_boot_xmldef");
    write_file(work / "routes.xml",
               "<protocol><message id=\"1\" name=\"ping\"/></protocol>");
    fs::path script = echo_script("shield_cov_boot_xmldef_svc.lua");
    uint16_t port = free_port();
    fs::path cfg = write_file(
        work / "app.yaml",
        "app:\n  name: cov\n"
        "actors:\n"
        "  - name: gw\n"
        "    script: " +
            script.string() +
            "\n    network:\n      tcp: 127.0.0.1:" + std::to_string(port) +
            "\n      protocol:\n"
            "        name: x\n"
            "        body:\n"
            "          codec: xmldef\n"
            "          catalog: routes.xml\n");
    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));

    fs::remove(work / "routes.xml");

    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket sock(io);
        boost::system::error_code ec;
        sock.connect(boost::asio::ip::tcp::endpoint(
                         boost::asio::ip::make_address("127.0.0.1"), port),
                     ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        boost::system::error_code ignore;
        sock.close(ignore);
    }

    shield::bootstrap::shutdown();
    fs::remove_all(work);
}

BOOST_AUTO_TEST_CASE(FullStackInitializeAndShutdown) {
    // --- scripts resolved via source_dir and lua.script_path ---
    fs::path work = base_dir("shield_cov_boot_full");
    fs::path scripts_dir = work / "scripts";
    fs::create_directories(scripts_dir);
    write_file(work / "echo_src.lua", "local M = {}\nreturn M\n");
    write_file(scripts_dir / "echo_path.lua", "local M = {}\nreturn M\n");

    fs::path console_dir = work / "console";
    fs::create_directories(console_dir);

    uint16_t gw_port = free_port();
    uint16_t warn_port = free_port();

    std::string yaml;
    yaml += "app:\n  name: cov\n";
    yaml += "net:\n  threads: 2\n";
    yaml += "lua:\n  script_path: " + scripts_dir.string() + "\n";
    yaml += "console:\n  enabled: true\n  socket_path: " +
            (console_dir / "console.sock").string() + "\n";
    yaml += "http:\n  enabled: true\n  host: 127.0.0.1\n  port: \"0\"\n";
    yaml += "actors:\n";
    // Resolved through the config file's directory (source_dir).
    yaml += "  - name: srcsvc\n    script: echo_src.lua\n";
    // Multi-instance naming (pathsvc.1 / pathsvc.2) via lua.script_path.
    yaml += "  - name: pathsvc\n    script: echo_path.lua\n    instances: 2\n";
    // Gateway with a builtin codec protocol and all listener limits set.
    yaml += "  - name: gateway\n    script: echo_path.lua\n    network:\n";
    yaml += "      tcp: 127.0.0.1:" + std::to_string(gw_port) + "\n";
    yaml += "      max_connections: 16\n      max_connections_per_ip: 8\n";
    yaml += "      max_frame_size: 65536\n      max_session_send_queue: 32\n";
    yaml += "      read_idle_timeout: 15000\n";
    yaml +=
        "      protocol:\n        name: cov\n        body:\n          codec: "
        "json\n";
    yaml += "        routes:\n          - id: 1\n            action: decode\n";
    // Non-loopback host only triggers a warning; the listener binds all IPv4.
    yaml += "  - name: warnhost\n    script: echo_path.lua\n    network:\n";
    yaml += "      tcp: 10.99.99.99:" + std::to_string(warn_port) + "\n";
    fs::path cfg = write_file(work / "app.yaml", yaml);

    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg.string()};
    rc.node_id = "cov-node";
    rc.num_workers = 2;
    rc.log_level = "warn";
    BOOST_REQUIRE(shield::bootstrap::initialize(rc));
    BOOST_CHECK(shield::bootstrap::is_initialized());

    // Drive a TCP connection so the per-connection protocol pipeline factory
    // runs, then send one complete framed request so the decoded packet is
    // dispatched through the gateway bridge's on_packet callback.
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket sock(io);
        boost::system::error_code ec;
        sock.connect(boost::asio::ip::tcp::endpoint(
                         boost::asio::ip::make_address("127.0.0.1"), gw_port),
                     ec);
        const std::string payload = R"({"route_id":1,"value":42})";
        const uint32_t be_len = static_cast<uint32_t>(payload.size());
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>(be_len >> 24));
        frame.push_back(static_cast<uint8_t>(be_len >> 16));
        frame.push_back(static_cast<uint8_t>(be_len >> 8));
        frame.push_back(static_cast<uint8_t>(be_len));
        frame.insert(frame.end(), payload.begin(), payload.end());
        boost::asio::write(sock, boost::asio::buffer(frame), ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        boost::system::error_code ignore;
        sock.close(ignore);
    }
    // Give the acceptor time to spin up the session pipeline.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Drive the console dispatcher end-to-end.
    {
        boost::asio::io_context io;
        boost::asio::local::stream_protocol::socket sock(io);
        boost::system::error_code ec;
        sock.connect(boost::asio::local::stream_protocol::endpoint(
                         (console_dir / "console.sock").string()),
                     ec);
        if (!ec) {
            boost::asio::write(sock,
                               boost::asio::buffer(std::string("help\n")));
            boost::asio::streambuf buf;
            boost::asio::read_until(sock, buf, '\n', ec);
        }
        boost::system::error_code ignore;
        sock.close(ignore);
    }

    shield::bootstrap::shutdown();
    BOOST_CHECK(!shield::bootstrap::is_initialized());
}

BOOST_AUTO_TEST_CASE(BootstrapRunDelegatesToShieldRun) {
    char arg0[] = "shield";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1, nullptr};
    BOOST_CHECK_EQUAL(shield::bootstrap::run(2, argv), 0);
}

BOOST_AUTO_TEST_SUITE_END()
