#include "shield/protocol/http_handler.hpp"

#include <algorithm>
#include <sstream>

#include "shield/log/logger.hpp"

namespace shield::protocol {

// HttpRouter implementation
void HttpRouter::add_route(const std::string& method,
                           const std::string& path_pattern,
                           RouteHandler handler) {
    Route route;
    route.method = method;
    route.path_regex = std::regex(path_pattern);
    route.handler = std::move(handler);
    routes_.push_back(std::move(route));
}

HttpResponse HttpRouter::route_request(const HttpRequest& request) {
    // Try to match routes
    for (const auto& route : routes_) {
        if (route.method == request.method &&
            std::regex_match(request.path, route.path_regex)) {
            return route.handler(request);
        }
    }

    // Default 404 response
    HttpResponse response;
    response.status_code = 404;
    response.status_text = "Not Found";
    response.body =
        R"({"error": "Not Found", "path": ")" + request.path + R"("})";
    return response;
}

// HttpProtocolHandler implementation
HttpProtocolHandler::HttpProtocolHandler() {
    // Set up default routes
    get_router().add_route("GET", "/health", [](const HttpRequest& req) {
        HttpResponse response;
        response.body = R"({"status": "healthy", "service": "shield"})";
        return response;
    });

    get_router().add_route("GET", "/status", [](const HttpRequest& req) {
        HttpResponse response;
        response.body = R"({"status": "running", "protocol": "http"})";
        return response;
    });
}

void HttpProtocolHandler::handle_data(uint64_t connection_id, const char* data,
                                      size_t length) {
    // Append data to connection buffer
    auto& buffer = connection_buffers_[connection_id];
    buffer.append(data, length);

    // Check if we have a complete HTTP request
    if (is_complete_request(buffer)) {
        try {
            // Parse HTTP request
            HttpRequest request = parse_http_request(buffer, connection_id);

            // Route the request
            HttpResponse response = router_.route_request(request);

            // Format and send response
            std::string response_str = format_http_response(response);
            send_data(connection_id, response_str);

            SHIELD_LOG_DEBUG << "HTTP " << request.method << " " << request.path
                             << " -> " << response.status_code;

        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "HTTP request parsing error: " << e.what();

            // Send 400 Bad Request
            HttpResponse error_response;
            error_response.status_code = 400;
            error_response.status_text = "Bad Request";
            error_response.body = R"({"error": "Bad Request"})";
            send_data(connection_id, format_http_response(error_response));
        }

        // Clear buffer after processing
        connection_buffers_.erase(connection_id);
    }
}

void HttpProtocolHandler::handle_connection(uint64_t connection_id) {
    SHIELD_LOG_DEBUG << "HTTP connection established: " << connection_id;
    connection_buffers_[connection_id] = "";
}

void HttpProtocolHandler::handle_disconnection(uint64_t connection_id) {
    SHIELD_LOG_DEBUG << "HTTP connection closed: " << connection_id;
    connection_buffers_.erase(connection_id);
}

bool HttpProtocolHandler::send_data(uint64_t connection_id,
                                    const std::string& data) {
    if (session_provider_) {
        auto session = session_provider_(connection_id);
        if (session) {
            session->send(data.c_str(), data.size());
            return true;
        }
    }
    return false;
}

void HttpProtocolHandler::set_session_provider(
    std::function<std::shared_ptr<net::Session>(uint64_t)> provider) {
    session_provider_ = std::move(provider);
}

HttpRequest HttpProtocolHandler::parse_http_request(
    const std::string& raw_request, uint64_t connection_id) {
    HttpRequest request;
    request.connection_id = connection_id;

    std::istringstream stream(raw_request);
    std::string line;

    // Parse request line
    if (std::getline(stream, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::istringstream request_line(line);
        request_line >> request.method >> request.path >> request.version;
    }

    // Parse headers
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        auto colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Trim whitespace
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            request.headers[key] = value;
        }
    }

    // Read body if Content-Length is specified
    auto content_length_it = request.headers.find("Content-Length");
    if (content_length_it != request.headers.end()) {
        size_t content_length = std::stoul(content_length_it->second);
        if (content_length > 0) {
            request.body.resize(content_length);
            stream.read(&request.body[0], content_length);
        }
    }

    return request;
}

std::string HttpProtocolHandler::format_http_response(
    const HttpResponse& response) {
    std::ostringstream stream;

    // Status line
    stream << "HTTP/1.1 " << response.status_code << " " << response.status_text
           << "\r\n";

    // Headers
    auto headers = response.headers;
    headers["Content-Length"] = std::to_string(response.body.size());

    for (const auto& [key, value] : headers) {
        stream << key << ": " << value << "\r\n";
    }

    // Empty line
    stream << "\r\n";

    // Body
    stream << response.body;

    return stream.str();
}

bool HttpProtocolHandler::is_complete_request(const std::string& data) {
    // Look for the end of headers (double CRLF)
    size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    // Check if we have the complete body
    std::istringstream stream(data);
    std::string line;
    std::unordered_map<std::string, std::string> headers;

    // Skip request line
    std::getline(stream, line);

    // Parse headers to find Content-Length
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        auto colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Trim whitespace
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            headers[key] = value;
        }
    }

    // Check Content-Length
    auto content_length_it = headers.find("Content-Length");
    if (content_length_it != headers.end()) {
        size_t content_length = std::stoul(content_length_it->second);
        size_t total_expected =
            header_end + 4 + content_length;  // +4 for "\r\n\r\n"
        return data.size() >= total_expected;
    }

    // No body expected
    return true;
}

// Factory function
std::unique_ptr<HttpProtocolHandler> create_http_handler() {
    return std::make_unique<HttpProtocolHandler>();
}

}  // namespace shield::protocol