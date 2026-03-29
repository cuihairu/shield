#include "shield/commands/diagnose_command.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <nlohmann/json.hpp>

#include "shield/config/config.hpp"
#include "shield/health/health_check.hpp"

namespace shield::commands {

namespace {
namespace beast = boost::beast;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct TcpEndpoint {
    std::string name;
    std::string host;
    std::string port;
};

struct ParsedHttpUrl {
    std::string host;
    std::string port;
};

bool parse_http_authority(const std::string& url, ParsedHttpUrl& out,
                          std::string& error) {
    std::string u = url;
    if (u.empty()) {
        error = "empty target";
        return false;
    }

    if (u.rfind("http://", 0) == 0) {
        u.erase(0, std::string("http://").size());
    } else if (u.rfind("https://", 0) == 0) {
        error = "https is not supported (use http)";
        return false;
    }

    auto slash_pos = u.find('/');
    if (slash_pos != std::string::npos) {
        u = u.substr(0, slash_pos);
    }

    if (u.empty()) {
        error = "missing host";
        return false;
    }

    std::string host = u;
    std::string port = "80";
    auto colon_pos = u.rfind(':');
    if (colon_pos != std::string::npos && colon_pos + 1 < u.size()) {
        host = u.substr(0, colon_pos);
        port = u.substr(colon_pos + 1);
    }
    if (host.empty()) {
        error = "missing host";
        return false;
    }

    out.host = std::move(host);
    out.port = std::move(port);
    return true;
}

bool tcp_probe(const std::string& host, const std::string& port,
               std::chrono::seconds timeout, std::string& error) {
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        stream.expires_after(timeout);
        auto const results = resolver.resolve(host, port);
        stream.connect(results);

        boost::system::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}  // namespace

DiagnoseCommand::DiagnoseCommand()
    : shield::cli::Command("diagnose",
                           "Runtime diagnostics and health checks") {
    setup_flags();
    set_long_description(
        "Perform runtime diagnostics, health checks, and system validation.")
        .set_usage("shield diagnose [OPTIONS]")
        .set_example(
            "  shield diagnose --health-check\\n"
            "  shield diagnose --connectivity\\n"
            "  shield diagnose --config-validation\\n"
            "  shield diagnose --benchmark --duration 30");
}

void DiagnoseCommand::setup_flags() {
    add_bool_flag("health-check", "Perform system health check", false);
    add_bool_flag("connectivity", "Test network connectivity", false);
    add_bool_flag("config-validation", "Validate configuration", false);
    add_bool_flag("benchmark", "Run performance benchmark", false);
    add_int_flag("duration", "Benchmark duration in seconds", 10);
    add_flag("format", "Output format (text, json)", "text");
    add_flag("target", "Target server URL for remote diagnostics", "");
}

int DiagnoseCommand::run(shield::cli::CommandContext& ctx) {
    auto& config_manager = shield::config::ConfigManager::instance();
    const std::string config_file = ctx.get_flag("config");

    std::cout << "Shield Runtime Diagnostics" << std::endl;

    auto try_load_config = [&]() -> bool {
        try {
            config_manager.load_config(config_file,
                                       shield::config::ConfigFormat::YAML);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to load configuration from '"
                      << config_file << "': " << e.what() << std::endl;
            return false;
        }
    };

    if (ctx.get_bool_flag("health-check")) {
        std::cout << "Running health check..." << std::endl;
        (void)try_load_config();

        auto& registry = shield::health::HealthCheckRegistry::instance();
        registry.clear_health_stats();

        registry.register_health_indicator(
            std::make_unique<shield::health::ApplicationHealthIndicator>());

        std::string disk_path = "/";
#ifdef _WIN32
        try {
            disk_path = std::filesystem::current_path().root_path().string();
            if (disk_path.empty()) {
                disk_path = "C:\\";
            }
        } catch (...) {
            disk_path = "C:\\";
        }
#else
        try {
            disk_path = std::filesystem::current_path().root_path().string();
            if (disk_path.empty()) {
                disk_path = "/";
            }
        } catch (...) {
            disk_path = "/";
        }
#endif
        registry.register_health_indicator(
            std::make_unique<shield::health::DiskSpaceHealthIndicator>(
                disk_path));

        try {
            const auto& tree = config_manager.get_config_tree();
            const std::string host =
                tree.get<std::string>("database.host", "");
            const int port = tree.get<int>("database.port", 0);
            const std::string name =
                tree.get<std::string>("database.name", "");
            const std::string user =
                tree.get<std::string>("database.username", "");

            if (!host.empty() && port > 0) {
                std::string conn;
                if (!name.empty() && !user.empty()) {
                    conn = "host=" + host + " port=" + std::to_string(port) +
                           " dbname=" + name + " user=" + user;
                } else {
                    conn = host + ":" + std::to_string(port);
                }
                registry.register_health_indicator(
                    std::make_unique<shield::health::DatabaseHealthIndicator>(
                        conn));
            }
        } catch (...) {
            // Optional: database config is not required for basic diagnostics.
        }

        const auto overall = registry.get_overall_health();
        const auto components = registry.get_all_health();

        const std::string format = ctx.get_flag("format");
        if (format == "json") {
            std::cout << shield::health::HealthEndpointBuilder::
                             build_json_response(overall, components, true)
                      << std::endl;
        } else {
            std::cout << shield::health::HealthEndpointBuilder::
                             build_health_response(overall, components, true)
                      << std::endl;
        }

        return overall.is_healthy() ? 0 : 1;
    }

    if (ctx.get_bool_flag("connectivity")) {
        std::cout << "Testing network connectivity..." << std::endl;
        (void)try_load_config();

        std::vector<TcpEndpoint> endpoints;

        // Optional remote target probe (HTTP authority).
        const std::string target = ctx.get_flag("target");
        if (!target.empty()) {
            ParsedHttpUrl parsed;
            std::string parse_error;
            if (!parse_http_authority(target, parsed, parse_error)) {
                std::cerr << "Invalid target '" << target
                          << "': " << parse_error << std::endl;
                return 1;
            }
            endpoints.push_back(
                {"target", parsed.host, parsed.port});
        }

        try {
            const auto& tree = config_manager.get_config_tree();
            const auto server_host =
                tree.get<std::string>("server.host", "");
            const int server_port = tree.get<int>("server.port", 0);
            if (!server_host.empty() && server_port > 0) {
                endpoints.push_back(
                    {"server", server_host, std::to_string(server_port)});
            }

            const auto db_host = tree.get<std::string>("database.host", "");
            const int db_port = tree.get<int>("database.port", 0);
            if (!db_host.empty() && db_port > 0) {
                endpoints.push_back(
                    {"database", db_host, std::to_string(db_port)});
            }

            const auto redis_host =
                tree.get<std::string>("redis.host", "");
            const int redis_port = tree.get<int>("redis.port", 0);
            if (!redis_host.empty() && redis_port > 0) {
                endpoints.push_back(
                    {"redis", redis_host, std::to_string(redis_port)});
            }
        } catch (...) {
            // Ignore: config might be absent; we'll just probe what's available.
        }

        if (endpoints.empty()) {
            std::cout << "No endpoints found to test." << std::endl;
            return 0;
        }

        bool all_ok = true;
        const auto timeout = std::chrono::seconds(2);

        for (const auto& ep : endpoints) {
            std::string err;
            const bool ok = tcp_probe(ep.host, ep.port, timeout, err);
            std::cout << ep.name << ": " << ep.host << ":" << ep.port
                      << " -> " << (ok ? "OK" : "FAIL") << std::endl;
            if (!ok) {
                all_ok = false;
                if (!err.empty()) {
                    std::cout << "  error: " << err << std::endl;
                }
            }
        }

        return all_ok ? 0 : 1;
    }

    if (ctx.get_bool_flag("config-validation")) {
        std::cout << "Validating configuration..." << std::endl;
        if (!try_load_config()) {
            return 1;
        }
        std::cout << "Configuration is valid: " << config_file << std::endl;
        return 0;
    }

    if (ctx.get_bool_flag("benchmark")) {
        int duration = ctx.get_int_flag("duration");
        std::cout << "Running performance benchmark for " << duration
                  << " seconds..." << std::endl;
        if (duration <= 0) {
            duration = 1;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(duration);

        std::string payload(1024, 'x');
        std::size_t iterations = 0;
        std::size_t bytes_processed = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            nlohmann::json j = {
                {"id", 1},
                {"name", "shield-bench"},
                {"payload", payload},
            };
            const std::string dumped = j.dump();
            const auto parsed = nlohmann::json::parse(dumped);
            (void)parsed;

            iterations++;
            bytes_processed += dumped.size();
        }

        const double secs = static_cast<double>(duration);
        const double iters_per_sec = iterations / secs;
        const double mb_per_sec =
            (bytes_processed / (1024.0 * 1024.0)) / secs;

        std::cout << "Iterations: " << iterations << std::endl;
        std::cout << "Throughput: " << iters_per_sec << " ops/s, "
                  << mb_per_sec << " MB/s" << std::endl;
        return 0;
    }

    print_help();
    return 0;
}

}  // namespace shield::commands
