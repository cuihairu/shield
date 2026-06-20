// [SHIELD_NET] HTTP client implementation using libcurl
#include "shield/net/http_client.hpp"

#include "shield/log/logger.hpp"

#include <curl/curl.h>

#include <mutex>

namespace shield::net {

namespace {
std::once_flag g_curl_init;
}  // namespace

void HttpClient::initialize() {
    std::call_once(g_curl_init, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

void HttpClient::cleanup() {
    curl_global_cleanup();
}

namespace {

// Callback for writing response body.
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// Callback for reading response headers.
size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::unordered_map<std::string, std::string>*>(userdata);
    std::string line(buffer, size * nitems);

    // Parse "Key: Value\r\n"
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        // Trim leading whitespace from value.
        size_t start = line.find_first_not_of(" \t", colon + 1);
        std::string value;
        if (start != std::string::npos) {
            // Remove trailing \r\n
            size_t end = line.find_last_not_of("\r\n");
            value = line.substr(start, end - start + 1);
        }
        (*headers)[key] = value;
    }
    return size * nitems;
}

}  // namespace

HttpClientResponse HttpClient::request(const HttpClientOptions& options) {
    HttpClientResponse response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to initialize curl handle";
        return response;
    }

    // Set URL.
    curl_easy_setopt(curl, CURLOPT_URL, options.url.c_str());

    // Set timeout.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(options.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(options.timeout_seconds));

    // Follow redirects.
    if (options.follow_redirects) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, static_cast<long>(options.max_redirects));
    }

    // Enable HTTP/2 if available.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    // Set method and body.
    if (options.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!options.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, options.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(options.body.size()));
        }
    } else if (options.method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!options.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, options.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(options.body.size()));
        }
    } else if (options.method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (options.method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (!options.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, options.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(options.body.size()));
        }
    }
    // GET is the default.

    // Set headers.
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : options.headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Set response callbacks.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    // SSL verification (enabled by default for security).
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // User agent.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Shield/1.0");

    // Perform request.
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        response.error = curl_easy_strerror(res);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status_code = static_cast<int>(status);
    }

    // Cleanup.
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);

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

HttpClientResponse HttpClient::patch_json(const std::string& url,
                                           const std::string& json_body,
                                           int timeout_seconds) {
    HttpClientOptions opts;
    opts.method = "PATCH";
    opts.url = url;
    opts.body = json_body;
    opts.headers["Content-Type"] = "application/json";
    opts.timeout_seconds = timeout_seconds;
    return request(opts);
}

HttpClientResponse HttpClient::upload(const std::string& url,
                                       const std::vector<HttpFileField>& files,
                                       const std::unordered_map<std::string, std::string>& fields,
                                       int timeout_seconds) {
    HttpClientResponse response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to initialize curl handle";
        return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeout_seconds));

    // Build multipart form.
    curl_mime* mime = curl_mime_init(curl);

    // Add form fields.
    for (const auto& [key, value] : fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, key.c_str());
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    }

    // Add file uploads.
    for (const auto& file : files) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, file.field_name.c_str());
        curl_mime_filedata(part, file.file_path.c_str());
        if (!file.content_type.empty()) {
            curl_mime_type(part, file.content_type.c_str());
        }
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    // Response callbacks.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Shield/1.0");

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        response.error = curl_easy_strerror(res);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status_code = static_cast<int>(status);
    }

    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    return response;
}

HttpClientResponse HttpClient::download(const std::string& url,
                                         const std::string& output_path,
                                         int timeout_seconds) {
    HttpClientResponse response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to initialize curl handle";
        return response;
    }

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) {
        response.error = "Failed to open output file: " + output_path;
        curl_easy_cleanup(curl);
        return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Shield/1.0");

    // Track headers for status code.
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        response.error = curl_easy_strerror(res);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status_code = static_cast<int>(status);
    }

    fclose(fp);
    curl_easy_cleanup(curl);

    // If download failed, remove the file.
    if (!response.ok()) {
        remove(output_path.c_str());
    }

    return response;
}

HttpClientResponse HttpClient::post_form(const std::string& url,
                                          const std::unordered_map<std::string, std::string>& fields,
                                          int timeout_seconds) {
    HttpClientOptions opts;
    opts.method = "POST";
    opts.url = url;
    opts.form_fields = fields;
    opts.headers["Content-Type"] = "application/x-www-form-urlencoded";
    opts.timeout_seconds = timeout_seconds;

    // Build URL-encoded form body.
    std::string form_body;
    for (const auto& [key, value] : fields) {
        if (!form_body.empty()) form_body += "&";
        // URL-encode key and value.
        CURL* curl = curl_easy_init();
        char* encoded_key = curl_easy_escape(curl, key.c_str(),
                                              static_cast<int>(key.size()));
        char* encoded_val = curl_easy_escape(curl, value.c_str(),
                                              static_cast<int>(value.size()));
        form_body += std::string(encoded_key) + "=" + std::string(encoded_val);
        curl_free(encoded_key);
        curl_free(encoded_val);
        curl_easy_cleanup(curl);
    }
    opts.body = form_body;

    return request(opts);
}

}  // namespace shield::net
