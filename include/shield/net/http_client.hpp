// [SHIELD_NET] HTTP client using Boost.Beast
#pragma once

#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <string>
#include <unordered_map>

namespace shield::net {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

/// @brief HTTP client response
struct HttpClientResponse {
    int status_code = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string error;
    bool ok() const { return status_code >= 200 && status_code < 400; }
};

/// @brief HTTP client request options
struct HttpClientOptions {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    int timeout_seconds = 10;
};

/// @brief Synchronous HTTP client for making outgoing requests.
///
/// Uses Boost.Beast + Boost.Asio (already integrated, no curl dependency).
/// Designed for:
/// - Webhook calls
/// - REST API consumption
/// - Health check probes
/// - Service-to-service HTTP calls
///
/// NOT designed for:
/// - High-throughput data transfer (use TCP/WebSocket)
/// - File uploads (use multipart library)
/// - Connection pooling (Phase 2)
class HttpClient {
public:
    /// @brief Make an HTTP request (synchronous, blocks until response)
    static HttpClientResponse request(const HttpClientOptions& options);

    /// @brief Convenience: GET request
    static HttpClientResponse get(const std::string& url,
                                  int timeout_seconds = 10);

    /// @brief Convenience: POST request with JSON body
    static HttpClientResponse post_json(const std::string& url,
                                        const std::string& json_body,
                                        int timeout_seconds = 10);

    /// @brief Convenience: PUT request with JSON body
    static HttpClientResponse put_json(const std::string& url,
                                       const std::string& json_body,
                                       int timeout_seconds = 10);

    /// @brief Convenience: DELETE request
    static HttpClientResponse del(const std::string& url,
                                  int timeout_seconds = 10);

private:
    /// @brief Parse URL into host, port, path
    static bool parse_url(const std::string& url,
                          std::string& host, uint16_t& port,
                          std::string& path, bool& is_https);
};

}  // namespace shield::net
