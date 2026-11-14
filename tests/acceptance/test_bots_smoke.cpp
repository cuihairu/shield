// tools/bots acceptance smoke: the real bots core drives the hello_world
// closure booted in-process — probe round trip, a small load plan, then all
// four chaos modes at small counts. The gateway must stay healthy through
// every injection (chaos report clean) and serve normal traffic afterwards.
#define BOOST_TEST_MODULE BotsSmoke
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "bot_client.hpp"
#include "chaos.hpp"
#include "load_runner.hpp"
#include "plan.hpp"
#include "shield/bootstrap/bootstrap.hpp"
#include "shield/shield.hpp"

#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif

namespace {

uint16_t free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acc(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    return acc.local_endpoint().port();
}

std::string scripts_dir() {
    return (std::filesystem::path(SHIELD_SOURCE_DIR) / "examples" /
            "hello_world" / "scripts")
        .generic_string();
}

// Same hello_world closure as test_client_rpc_e2e (auth on an ephemeral
// listener, player post-bind, room serving moves).
std::filesystem::path write_config(uint16_t port) {
    namespace fs = std::filesystem;
    const std::string scripts = scripts_dir();
    const fs::path cfg_path =
        fs::temp_directory_path() / "shield_bots_smoke.yaml";
    std::ofstream out(cfg_path, std::ios::trunc);
    out << "app:\n"
           "  name: hello_world_bots\n"
           "log:\n"
           "  level: \"error\"\n"
           "  console: true\n"
           "actors:\n"
           "  - name: \"auth\"\n"
           "    script: \"" +
               scripts +
               "/auth.lua\"\n"
               "    instances: 1\n"
               "    network:\n"
               "      tcp: \"127.0.0.1:" +
               std::to_string(port) +
               "\"\n"
               "      protocol:\n"
               "        name: hello.v1\n"
               "        envelope:\n"
               "          type: \"idlen\"\n"
               "          route_id_bytes: 2\n"
               "          length_bytes: 2\n"
               "          endian: \"big\"\n"
               "        body:\n"
               "          codec: \"json\"\n"
               "    rpc:\n"
               "      routes:\n"
               "        - id: 1\n"
               "          name: \"login\"\n"
               "          binding: \"login\"\n"
               "          direction: \"c2s\"\n"
               "          requires_auth: false\n"
               "        - id: 100\n"
               "          name: \"login_result\"\n"
               "          binding: \"login_result\"\n"
               "          direction: \"s2c\"\n"
               "  - name: \"player\"\n"
               "    script: \"" +
               scripts +
               "/player.lua\"\n"
               "    instances: 1\n"
               "    rpc:\n"
               "      routes:\n"
               "        - id: 2\n"
               "          name: \"move\"\n"
               "          binding: \"move\"\n"
               "          direction: \"c2s\"\n"
               "          requires_auth: true\n"
               "  - name: \"room\"\n"
               "    script: \"" +
               scripts +
               "/room.lua\"\n"
               "    instances: 1\n"
               "    rpc:\n"
               "      routes:\n"
               "        - id: 200\n"
               "          name: \"move_result\"\n"
               "          binding: \"move_result\"\n"
               "          direction: \"s2c\"\n";
    return cfg_path;
}

}  // namespace

BOOST_AUTO_TEST_CASE(ProbeLoadAndChaosAgainstLiveClosure) {
    namespace fs = std::filesystem;

    const uint16_t port = free_port();
    const auto cfg_path = write_config(port);
    const std::string target = "127.0.0.1:" + std::to_string(port);

    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg_path.string()};
    BOOST_REQUIRE_MESSAGE(shield::bootstrap::initialize(rc),
                          "bootstrap initialize failed");

    const auto timeout = std::chrono::milliseconds(5000);

    // --- 1. probe: login + move round trip ---
    const auto probe =
        shield::bots::probe_hello_world(target, "smoke-probe", timeout);
    BOOST_REQUIRE_MESSAGE(probe.ok, "probe failed: " << probe.detail);

    // --- 2. load: 4 bots x 2s, sequential move loop ---
    shield::bots::LoadPlan plan;
    plan.name = "bots-smoke";
    plan.target = target;
    plan.duration_seconds = 2.0;
    plan.connections = 4;
    plan.requests_per_second = 10.0;
    plan.timeout_ms = 5000;
    plan.login_player_prefix = "smoke-bot";
    shield::bots::WorkRoute move;
    move.route_id = 2;
    move.reply_route_id = 200;
    move.weight = 1;
    move.body = nlohmann::json{{"x", 5}, {"y", 6}};
    plan.work_routes.push_back(move);

    const auto summary = shield::bots::run_load(plan);
    BOOST_CHECK_EQUAL(summary.connections_failed, 0);
    BOOST_CHECK_EQUAL(summary.login_failures, 0);
    const auto replies = summary.metrics.routes().find(200);
    BOOST_REQUIRE_MESSAGE(
        replies != summary.metrics.routes().end() &&
            replies->second.replies > 0,
        "load produced zero move replies\n" + summary.report());

    // --- 3. chaos: every mode at small counts; the closure must survive ---
    for (const auto& mode : shield::bots::chaos_mode_names()) {
        shield::bots::ChaosOptions options;
        options.target = target;
        options.count = 2;
        options.player_prefix = "smoke-chaos";
        const auto report = shield::bots::run_chaos(mode, options);
        std::string transcript;
        for (const auto& line : report.lines) {
            transcript += line + "\n";
        }
        BOOST_CHECK_MESSAGE(
            report.clean(),
            "chaos mode " + mode + " found anomalies:\n" + transcript);
    }

    // --- 4. the gateway still serves a normal round trip after chaos ---
    const auto after =
        shield::bots::probe_hello_world(target, "smoke-after", timeout);
    BOOST_CHECK_MESSAGE(after.ok, "probe after chaos failed: " << after.detail);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    shield::bootstrap::shutdown();
    std::filesystem::remove(cfg_path);
}
