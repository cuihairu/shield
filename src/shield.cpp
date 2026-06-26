// [SHIELD] Public top-level runtime entry point
#include "shield/shield.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "shield/bootstrap/bootstrap.hpp"
#include "shield/version.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace shield {
namespace {

struct CliOptions {
    std::vector<std::string> config_files{"config/app.yaml"};
    std::string log_level = "info";
    int workers = 0;
    bool show_help = false;
    bool show_version = false;
    bool check_config = false;
    bool has_node_id = false;
    std::string node_id;
    bool parse_error = false;
    std::string error;
};

std::atomic<bool> g_stop_requested{false};

void request_stop() { g_stop_requested.store(true); }

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
        signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
        request_stop();
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) { request_stop(); }
#endif

void install_signal_handlers() {
    g_stop_requested.store(false);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif
}

void uninstall_signal_handlers() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, FALSE);
#else
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
#endif
}

void print_help(const char* executable) {
    const char* name =
        (executable && executable[0] != '\0') ? executable : "shield";
    std::cout
        << "Shield Game Server Runtime\n\n"
        << "Usage: " << name << " [options]\n\n"
        << "Options:\n"
        << "  --config, -c <file>    Config file path; may be repeated\n"
        << "  --log-level <level>    Log level override (debug, info, warn, "
           "error)\n"
        << "  --workers <n>          Worker thread count; 0 means auto\n"
        << "  --node-id <id>         Cluster node id; requires shield_cluster\n"
        << "  --check-config         Initialize, validate, then shut down\n"
        << "  --version, -v          Show version information\n"
        << "  --help, -h             Show this help message\n";
}

CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    bool saw_config = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";

        auto require_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                options.parse_error = true;
                options.error = std::string("missing value for ") + name;
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            return options;
        }
        if (arg == "--version" || arg == "-v") {
            options.show_version = true;
            return options;
        }
        if (arg == "--config" || arg == "-c") {
            const char* value = require_value(arg.c_str());
            if (options.parse_error) {
                return options;
            }
            if (!saw_config) {
                options.config_files.clear();
                saw_config = true;
            }
            options.config_files.emplace_back(value);
            continue;
        }
        if (arg == "--log-level") {
            const char* value = require_value("--log-level");
            if (options.parse_error) {
                return options;
            }
            options.log_level = value;
            continue;
        }
        if (arg == "--workers") {
            const char* value = require_value("--workers");
            if (options.parse_error) {
                return options;
            }
            try {
                options.workers = std::stoi(value);
            } catch (const std::exception&) {
                options.parse_error = true;
                options.error = "invalid integer for --workers";
                return options;
            }
            if (options.workers < 0) {
                options.parse_error = true;
                options.error = "--workers must be >= 0";
                return options;
            }
            continue;
        }
        if (arg == "--node-id") {
            const char* value = require_value("--node-id");
            if (options.parse_error) {
                return options;
            }
            options.node_id = value;
            options.has_node_id = true;
            continue;
        }
        if (arg == "--check-config") {
            options.check_config = true;
            continue;
        }

        options.parse_error = true;
        options.error = "unknown argument: " + arg;
        return options;
    }

#ifndef SHIELD_ENABLE_CLUSTER
    if (options.has_node_id) {
        options.parse_error = true;
        options.error =
            "--node-id requires shield_cluster, which is not enabled";
    }
#endif

    return options;
}

void wait_for_stop() {
    while (!g_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace

int run(int argc, char** argv) {
    try {
        CliOptions options = parse_cli(argc, argv);

        if (options.show_help) {
            print_help(argc > 0 ? argv[0] : "shield");
            return 0;
        }
        if (options.show_version) {
            shield::print_version();
            return 0;
        }
        if (options.parse_error) {
            std::cerr << "shield: " << options.error << '\n';
            std::cerr << "Try '--help' for usage.\n";
            return 1;
        }

        bootstrap::RuntimeConfig config;
        config.config_files = options.config_files;
        config.config_file =
            options.config_files.empty() ? "" : options.config_files.front();
        config.log_level = options.log_level;
        config.num_workers = options.workers;
        config.node_id = options.node_id;

        install_signal_handlers();

        if (!bootstrap::initialize(config)) {
            uninstall_signal_handlers();
            return 1;
        }

        if (options.check_config) {
            bootstrap::shutdown();
            uninstall_signal_handlers();
            return 0;
        }

        std::cout << "Shield runtime running (press Ctrl+C to stop)"
                  << std::endl;
        wait_for_stop();
        bootstrap::shutdown();
        uninstall_signal_handlers();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 2;
    }
}

}  // namespace shield
