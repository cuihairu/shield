// tools/bots/main.cpp - CLI: probe | run | chaos.
//
//   bots probe --target 127.0.0.1:8001
//   bots run --plan plans/hall_load.yaml [--target 127.0.0.1:8001]
//   bots chaos --target 127.0.0.1:8001 [--mode all] [--count 8]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "bot_client.hpp"
#include "chaos.hpp"
#include "load_runner.hpp"
#include "plan.hpp"

namespace {

#ifndef SHIELD_BOTS_VERSION_STRING
#define SHIELD_BOTS_VERSION_STRING "dev"
#endif

void usage(std::FILE* out) {
    std::fprintf(
        out,
        "bots %s — load tester / chaos injector for the shield "
        "gateway\n\n"
        "usage:\n"
        "  bots probe --target HOST:PORT [--timeout MS] [--player ID]\n"
        "  bots run --plan FILE.yaml [--target HOST:PORT]\n"
        "  bots chaos --target HOST:PORT [--mode all|bad_frame|"
        "oversize|route_flood|disconnect_flood] [--count N]\n"
        "  bots --help | --version\n",
        SHIELD_BOTS_VERSION_STRING);
}

bool take_value(int argc, char** argv, const char* flag, std::string& out) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0 && i + 1 < argc) {
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

int cmd_probe(int argc, char** argv) {
    std::string target = "127.0.0.1:8001";
    std::string player = "probe-cli";
    std::string timeout_text;
    take_value(argc, argv, "--target", target);
    take_value(argc, argv, "--player", player);
    int timeout_ms = 2000;
    if (take_value(argc, argv, "--timeout", timeout_text)) {
        timeout_ms = std::stoi(timeout_text);
    }
    auto result = shield::bots::probe_hello_world(
        target, player, std::chrono::milliseconds(timeout_ms));
    if (result.ok) {
        std::printf("probe ok: login %.2fms move %.2fms\n", result.login_ms,
                    result.move_ms);
        return 0;
    }
    std::printf("probe FAILED: %s\n", result.detail.c_str());
    return 2;
}

int cmd_run(int argc, char** argv) {
    std::string plan_path;
    if (!take_value(argc, argv, "--plan", plan_path)) {
        std::fprintf(stderr, "run: missing --plan FILE.yaml\n");
        return 1;
    }
    shield::bots::LoadPlan plan;
    try {
        plan = shield::bots::load_plan(plan_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "run: %s\n", e.what());
        return 1;
    }
    std::string target;
    if (take_value(argc, argv, "--target", target)) {
        plan.target = target;
    }
    const auto summary = shield::bots::run_load(plan);
    std::fputs(summary.report().c_str(), stdout);
    if (summary.connections_failed > 0 || summary.login_failures > 0) {
        return 1;
    }
    // Every reply-carrying route must have gotten at least one reply.
    for (const auto& route : plan.work_routes) {
        if (route.reply_route_id != 0) {
            const auto found =
                summary.metrics.routes().find(route.reply_route_id);
            if (found == summary.metrics.routes().end() ||
                found->second.replies == 0) {
                std::fprintf(stderr, "run: route %u produced zero replies\n",
                             route.reply_route_id);
                return 1;
            }
        }
    }
    return 0;
}

int cmd_chaos(int argc, char** argv) {
    shield::bots::ChaosOptions options;
    std::string mode = "all";
    take_value(argc, argv, "--target", options.target);
    take_value(argc, argv, "--mode", mode);
    std::string count_text;
    if (take_value(argc, argv, "--count", count_text)) {
        options.count = std::stoi(count_text);
    }
    shield::bots::ChaosReport report;
    try {
        report = shield::bots::run_chaos(mode, options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "chaos: %s\n", e.what());
        return 1;
    }
    for (const auto& line : report.lines) {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
    }
    if (report.clean()) {
        std::printf("chaos %s: no anomalies\n", mode.c_str());
        return 0;
    }
    std::printf("chaos %s: %zu anomaly/anomalies\n", mode.c_str(),
                report.anomalies.size());
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        usage(stdout);
        return 0;
    }
    if (command == "--version") {
        std::printf("bots %s\n", SHIELD_BOTS_VERSION_STRING);
        return 0;
    }
    if (command == "probe") {
        return cmd_probe(argc, argv);
    }
    if (command == "run") {
        return cmd_run(argc, argv);
    }
    if (command == "chaos") {
        return cmd_chaos(argc, argv);
    }
    usage(stderr);
    return 1;
}
