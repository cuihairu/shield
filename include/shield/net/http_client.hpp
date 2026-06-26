// [SHIELD_NET] HTTP client using libcurl
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace shield::net {

/// @brief HTTP client response
struct HttpClientResponse {
    int status_code = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string error;
    bool ok() const { return status_code >= 200 && status_code < 400; }
};

/// @brief File upload entry for multipart/form-data
struct HttpFileField {
    std::string field_name;    // Form field name
    std::string file_path;     // Local file path to upload
    std::string content_type;  // MIME type (e.g. "image/png")
};

/// @brief HTTP client request options
struct HttpClientOptions {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> form_fields;  // form data
    std::vector<HttpFileField> files;  // multipart uploads
    int timeout_seconds = 10;
    bool follow_redirects = true;
    int max_redirects = 5;
    // Auth
    std::string auth_bearer;          // Bearer token
    std::string auth_basic_user;      // Basic auth username
    std::string auth_basic_password;  // Basic auth password
    // Proxy
    std::string proxy;  // proxy URL (e.g. "http://proxy:8080")
    // SSL
    bool verify_ssl = true;
    std::string ca_cert_path;  // custom CA cert file
    // Retry
    int retry_count = 0;        // number of retries on failure
    int retry_delay_ms = 1000;  // delay between retries
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
/// - File upload (multipart/form-data)
/// - File download
/// - Form data (application/x-www-form-urlencoded)
/// - Bearer / Basic auth
/// - Retry on failure
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

    /// @brief Upload files via multipart/form-data POST
    static HttpClientResponse upload(
        const std::string& url, const std::vector<HttpFileField>& files,
        const std::unordered_map<std::string, std::string>& fields = {},
        int timeout_seconds = 60);

    /// @brief Download a file to disk
    static HttpClientResponse download(const std::string& url,
                                       const std::string& output_path,
                                       int timeout_seconds = 60);

    /// @brief POST form data (application/x-www-form-urlencoded)
    static HttpClientResponse post_form(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& fields,
        int timeout_seconds = 10);
};

}  // namespace shield::net
