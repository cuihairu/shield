#include "shield/commands/cli_command.hpp"

#include <iostream>
#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include "shield/config/config.hpp"

namespace shield::commands {

namespace {
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct ParsedHttpUrl {
    std::string host;
    std::string port;
    std::string target;
};

bool parse_http_url(const std::string& url, ParsedHttpUrl& out,
                    std::string& error) {
    std::string u = url;
    if (u.empty()) {
        error = "empty url";
        return false;
    }

    // Strip scheme.
    if (u.rfind("http://", 0) == 0) {
        u.erase(0, std::string("http://").size());
    } else if (u.rfind("https://", 0) == 0) {
        error = "https is not supported (use http)";
        return false;
    }

    // Split authority and target.
    std::string authority;
    std::string target = "/";
    auto slash_pos = u.find('/');
    if (slash_pos == std::string::npos) {
        authority = u;
    } else {
        authority = u.substr(0, slash_pos);
        target = u.substr(slash_pos);
        if (target.empty()) {
            target = "/";
        }
    }

    if (authority.empty()) {
        error = "missing host";
        return false;
    }

    // Split host and port.
    std::string host = authority;
    std::string port = "80";
    auto colon_pos = authority.rfind(':');
    if (colon_pos != std::string::npos && colon_pos + 1 < authority.size()) {
        host = authority.substr(0, colon_pos);
        port = authority.substr(colon_pos + 1);
    }

    if (host.empty()) {
        error = "missing host";
        return false;
    }

    out.host = std::move(host);
    out.port = std::move(port);
    out.target = std::move(target);
    return true;
}

struct HttpResult {
    int status = 0;
    std::string reason;
    std::string body;
};

HttpResult http_get(const ParsedHttpUrl& url, std::chrono::seconds timeout) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);
    stream.expires_after(timeout);

    auto const results = resolver.resolve(url.host, url.port);
    stream.connect(results);

    http::request<http::string_body> req{http::verb::get, url.target, 11};
    req.set(http::field::host, url.host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    HttpResult out;
    out.status = res.result_int();
    out.reason = res.reason();
    out.body = std::move(res.body());
    return out;
}
}  // namespace

CLICommand::CLICommand()
    : shield::cli::Command(
          "cli", "Command line interface for Shield server management") {
    setup_flags();
    set_long_description(
        "Connect to a running Shield server and execute management commands.")
        .set_usage("shield cli [OPTIONS] [COMMAND]")
        .set_example(
            "  shield cli --url http://localhost:8080\\n"
            "  shield cli --url remote.server.com --timeout 30");
}

void CLICommand::setup_flags() {
    add_flag("url", "Server URL", "http://localhost:8080");
    add_int_flag("timeout", "Request timeout in seconds", 30);
    add_flag("format", "Output format (text, json)", "text");
    add_bool_flag("verbose", "Verbose output", false);
}

int CLICommand::run(shield::cli::CommandContext& ctx) {
    // Load CLI-specific layered configuration
    auto& config_manager = shield::config::ConfigManager::instance();
    try {
        config_manager.load_config(ctx.get_flag("config"),
                                   shield::config::ConfigFormat::YAML);
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to load CLI configuration: " << e.what()
                  << std::endl;
        // Continue with defaults
    }

    std::cout << "Shield CLI Client" << std::endl;

    // Use configuration with command line overrides
    std::string server_url = ctx.get_flag("url");
    if (server_url.empty() || server_url == "http://localhost:8080") {
        try {
            const auto& config_tree = config_manager.get_config_tree();
            server_url = config_tree.get<std::string>("client.default_url",
                                                      "http://localhost:8080");
        } catch (...) {
            server_url = "http://localhost:8080";  // fallback
        }
    }

    int timeout = ctx.get_int_flag("timeout");
    if (timeout == 30) {  // default value
        try {
            const auto& config_tree = config_manager.get_config_tree();
            timeout = config_tree.get<int>("client.timeout", 30);
        } catch (...) {
            timeout = 30;  // fallback
        }
    }

    std::cout << "Connecting to: " << server_url << std::endl;
    std::cout << "Timeout: " << timeout << "s" << std::endl;
    std::cout << "Format: " << ctx.get_flag("format") << std::endl;

    if (ctx.get_bool_flag("verbose")) {
        std::cout << "Verbose mode enabled" << std::endl;
    }

    std::string command = ctx.arg(0);
    if (command.empty()) {
        command = "health";
    }

    std::string override_target;
    if (command == "health") {
        override_target = "/health";
    } else if (command == "metrics") {
        override_target = "/metrics";
    } else if (!command.empty() && command.front() == '/') {
        override_target = command;
    } else {
        // Treat unknown command as a path.
        override_target = "/" + command;
    }

    ParsedHttpUrl parsed;
    std::string parse_error;
    if (!parse_http_url(server_url, parsed, parse_error)) {
        std::cerr << "Invalid url '" << server_url
                  << "': " << parse_error << std::endl;
        return 1;
    }

    if (parsed.target == "/" && !override_target.empty()) {
        parsed.target = override_target;
    }

    try {
        const auto res = http_get(parsed, std::chrono::seconds(timeout));
        if (ctx.get_flag("format") == "json") {
            std::cout << res.body << std::endl;
        } else {
            std::cout << "HTTP " << res.status << " " << res.reason
                      << std::endl;
            if (!res.body.empty()) {
                std::cout << res.body << std::endl;
            }
        }
        return (res.status >= 200 && res.status < 400) ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Request failed: " << e.what() << std::endl;
        return 1;
    }
}

}  // namespace shield::commands
