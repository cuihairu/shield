// [SHIELD_NET] HTTP client implementation using Boost.Beast
#include "shield/net/http_client.hpp"

#include "shield/log/logger.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

namespace shield::net {

bool HttpClient::parse_url(const std::string& url,
                           std::string& host, uint16_t& port,
                           std::string& path, bool& is_https) {
    // Simple URL parser: http(s)://host[:port]/path
    is_https = false;
    std::string remaining = url;

    if (remaining.substr(0, 8) == "https://") {
        is_https = true;
        remaining = remaining.substr(8);
    } else if (remaining.substr(0, 7) == "http://") {
        remaining = remaining.substr(7);
    } else {
        return false;  // Invalid URL
    }

    // Split host:port/path
    auto slash_pos = remaining.find('/');
    std::string host_port;
    if (slash_pos != std::string::npos) {
        host_port = remaining.substr(0, slash_pos);
        path = remaining.substr(slash_pos);
    } else {
        host_port = remaining;
        path = "/";
    }

    // Split host:port
    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        host = host_port.substr(0, colon_pos);
        try {
            port = static_cast<uint16_t>(std::stoi(host_port.substr(colon_pos + 1)));
        } catch (...) {
            return false;
        }
    } else {
        host = host_port;
        port = is_https ? 443 : 80;
    }

    return !host.empty();
}

HttpClientResponse HttpClient::request(const HttpClientOptions& options) {
    HttpClientResponse response;

    std::string host;
    uint16_t port;
    std::string path;
    bool is_https;

    if (!parse_url(options.url, host, port, path, is_https)) {
        response.error = "invalid URL: " + options.url;
        return response;
    }

    if (is_https) {
        response.error = "HTTPS not supported in Phase 1 (use HTTP or proxy)";
        return response;
    }

    try {
        net::io_context ioc;
        net::ip::tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Resolve host.
        auto results = resolver.resolve(host, std::to_string(port));

        // Connect with timeout.
        stream.connect(results);

        // Build HTTP request.
        http::verb verb = http::verb::get;
        if (options.method == "POST") verb = http::verb::post;
        else if (options.method == "PUT") verb = http::verb::put;
        else if (options.method == "DELETE") verb = http::verb::delete_;
        else if (options.method == "PATCH") verb = http::verb::patch;

        http::request<http::string_body> req{verb, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "Shield/1.0");

        for (const auto& [key, value] : options.headers) {
            req.set(key, value);
        }

        if (!options.body.empty()) {
            req.body() = options.body;
            req.prepare_payload();
        }

        // Send request.
        stream.expires_after(std::chrono::seconds(options.timeout_seconds));
        http::write(stream, req);

        // Read response.
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        // Parse response.
        response.status_code = static_cast<int>(res.result());
        response.body = res.body();
        for (auto const& field : res) {
            response.headers[std::string(field.name_string())] =
                std::string(field.value());
        }

        // Graceful close.
        stream.socket().shutdown(net::ip::tcp::socket::shutdown_both);

    } catch (const beast::system_error& e) {
        response.error = e.what();
    } catch (const std::exception& e) {
        response.error = e.what();
    }

    return response;
}

HttpClientResponse HttpClient::get(const std::string& url, int timeout_seconds) {
    HttpClientOptions opts;
    opts.method = "GET";
    opts.url = url;
    opts.timeout_seconds = timeout_seconds;
    return request(opts);
}

HttpClientResponse HttpClient::post_json(const std::string& url,
                                          const std::string& json_body,
                                          int timeout_seconds) {
    HttpClientOptions opts;
    opts.method = "POST";
    opts.url = url;
    opts.body = json_body;
    opts.headers["Content-Type"] = "application/json";
    opts.timeout_seconds = timeout_seconds;
    return request(opts);
}

HttpClientResponse HttpClient::put_json(const std::string& url,
                                         const std::string& json_body,
                                         int timeout_seconds) {
    HttpClientOptions opts;
    opts.method = "PUT";
    opts.url = url;
    opts.body = json_body;
    opts.headers["Content-Type"] = "application/json";
    opts.timeout_seconds = timeout_seconds;
    return request(opts);
}

HttpClientResponse HttpClient::del(const std::string& url, int timeout_seconds) {
    HttpClientOptions opts;
    opts.method = "DELETE";
    opts.url = url;
    opts.timeout_seconds = timeout_seconds;
    return request(opts);
}

}  // namespace shield::net
