// [SHIELD_NET] HTTP server using Boost.Beast
#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace shield::net {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

/// @brief HTTP request type
using HttpRequest = http::request<http::string_body>;

/// @brief HTTP response type
using HttpResponse = http::response<http::string_body>;

/// @brief HTTP request handler function
using HttpHandler = std::function<HttpResponse(const HttpRequest& req)>;

/// @brief HTTP route method
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE_,
    PATCH,
    ANY,
};

/// @brief HTTP server configuration
struct HttpServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    size_t max_connections = 1000;
    int request_timeout_seconds = 30;
};

/// @brief Lightweight HTTP server for game management / API endpoints.
///
/// Designed for:
/// - Health checks
/// - Metrics endpoints
/// - Admin/ops APIs
/// - Webhook receivers
/// - REST APIs for game data
///
/// NOT designed for:
/// - High-throughput game traffic (use TCP/WebSocket instead)
/// - Static file serving (use nginx/caddy in front)
/// - Full web framework features
class HttpServer {
public:
    explicit HttpServer(const HttpServerConfig& config = {});
    ~HttpServer();

    // Non-copyable
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// @brief Register a route handler
    /// @param method HTTP method (GET, POST, etc.)
    /// @param path URL path (e.g., "/api/health"). A path segment starting
    ///        with ':' (e.g. "/api/users/:id") matches any single segment and
    ///        captures it as a path parameter.
    /// @param handler Handler function
    /// @note Thread-safe: routes may be registered while the server runs.
    void route(HttpMethod method, const std::string& path, HttpHandler handler);

    /// @brief Convenience: register a GET handler
    void get(const std::string& path, HttpHandler handler);

    /// @brief Convenience: register a POST handler
    void post(const std::string& path, HttpHandler handler);

    /// @brief Start listening for HTTP connections
    void start();

    /// @brief Stop the server
    void stop();

    /// @brief Check if server is running
    bool is_running() const;

    /// @brief Get the configured port
    uint16_t port() const { return config_.port; }

    /// @brief Set a default handler for unmatched routes
    void set_default_handler(HttpHandler handler);

private:
    struct RouteKey {
        HttpMethod method;
        std::string path;

        bool operator==(const RouteKey& other) const {
            return method == other.method && path == other.path;
        }
    };

    struct RouteKeyHash {
        size_t operator()(const RouteKey& k) const {
            return std::hash<int>{}(static_cast<int>(k.method)) ^
                   (std::hash<std::string>{}(k.path) << 1);
        }
    };

    // One in-flight connection. Kept alive through shared_from_this while
    // async operations are pending; released (closing the socket) when the
    // connection errors, hits EOF, or is done after a non-keep-alive
    // response.
    struct Session;

    // Match a request method/path against the route table. Tries an exact
    // match first, then falls back to ':' parameter patterns. Returns the
    // handler under lock (copied out so it runs outside the mutex).
    std::shared_ptr<const HttpHandler> match_route(
        HttpMethod method, const std::string& path) const;

    // Shared request-processing pipeline used by every read callback.
    HttpResponse dispatch(const HttpRequest& req) const;

    void do_accept();
    void handle_session(std::shared_ptr<net::ip::tcp::socket> socket);

    HttpServerConfig config_;
    mutable std::mutex routes_mutex_;
    std::unordered_map<RouteKey, std::shared_ptr<const HttpHandler>,
                       RouteKeyHash>
        routes_;
    HttpHandler default_handler_;

    net::io_context io_context_;
    std::unique_ptr<net::ip::tcp::acceptor> acceptor_;
    bool running_ = false;
    std::thread io_thread_;
};

}  // namespace shield::net
