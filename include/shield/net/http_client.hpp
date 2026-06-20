// [SHIELD_NET] HTTP client using libcurl
#pragma once

#include <string>
#include <unordered_map>

namespace shield::net {

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
    bool follow_redirects = true;
    int max_redirects = 5;
};

/// @brief HTTP client using libcurl.
///
/// Supports:
/// - HTTP/1.1 and HTTP/2 (via nghttp2)
/// - HTTPS (via OpenSSL/Schannel)
/// - Connection pooling
/// - Redirects
/// - Cookies
/// - Proxy
///
/// Designed for:
/// - Payment API calls (HTTPS mandatory)
/// - Webhook calls
/// - REST API consumption
/// - Service-to-service HTTP calls
class HttpClient {
public:
    /// @brief Initialize the curl global state (call once at startup)
    static void initialize();

    /// @brief Cleanup curl global state (call at shutdown)
    static void cleanup();

    /// @brief Make an HTTP request
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

    /// @brief Convenience: PATCH request with JSON body
    static HttpClientResponse patch_json(const std::string& url,
                                         const std::string& json_body,
                                         int timeout_seconds = 10);
};

}  // namespace shield::net
