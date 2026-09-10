#define BOOST_TEST_MODULE CovShield
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

BOOST_AUTO_TEST_SUITE_END()
