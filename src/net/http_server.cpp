// [SHIELD_NET] HTTP server implementation using Boost.Beast
#include "shield/net/http_server.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <cctype>
#include <vector>

#include "shield/log/logger.hpp"

namespace shield::net {

namespace {

// Split "/a/b/c" into segments, ignoring empty parts (leading/trailing '/').
std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segments;
    size_t pos = 0;
    while (pos < path.size()) {
        const size_t next = path.find('/', pos);
        const std::string seg = path.substr(
            pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!seg.empty()) {
            segments.push_back(seg);
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return segments;
}  // GCOVR_EXCL_LINE (unwind cleanup for a throwing push_back; untestable)

// True when a ':'-parameter route pattern matches the concrete path.
bool pattern_matches(const std::vector<std::string>& pattern,
                     const std::vector<std::string>& path) {
    if (pattern.size() != path.size()) {
        return false;
    }
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i].empty() || pattern[i][0] != ':') {
            if (pattern[i] != path[i]) {
                return false;
            }
        }
    }
    return true;
}

// Case-insensitive substring check for a Connection-header token.
bool connection_token_exists(const std::string& value, const char* token) {
    std::string lower;
    lower.reserve(value.size());
    for (char c : value) {
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower.find(token) != std::string::npos;
}

// Should the connection stay open after writing this response? An
// explicit Connection header wins; otherwise HTTP/1.1 keeps the connection
// alive and HTTP/1.0 closes it (mirrors beast's fields::keep_alive logic
// evaluated against the request version).
bool connection_keeps_alive(const HttpResponse& response,
                            unsigned request_version) {
    auto it = response.base().find(http::field::connection);
    if (request_version < 11) {
        return it != response.base().end() &&
               connection_token_exists(std::string(it->value()), "keep-alive");
    }
    return it == response.base().end() ||
           !connection_token_exists(std::string(it->value()), "close");
}

HttpMethod verb_to_method(http::verb verb) {
    switch (verb) {
        case http::verb::get:
            return HttpMethod::GET;
        case http::verb::post:
            return HttpMethod::POST;
        case http::verb::put:
            return HttpMethod::PUT;
        case http::verb::delete_:
            return HttpMethod::DELETE_;
        case http::verb::patch:
            return HttpMethod::PATCH;
        default:
            return HttpMethod::ANY;
    }
}

}  // namespace

HttpServer::HttpServer(const HttpServerConfig& config) : config_(config) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(HttpMethod method, const std::string& path,
                       HttpHandler handler) {
    auto stored = std::make_shared<const HttpHandler>(std::move(handler));
    std::lock_guard<std::mutex> lock(routes_mutex_);
    routes_[{method, path}] = std::move(stored);
}

void HttpServer::get(const std::string& path, HttpHandler handler) {
    route(HttpMethod::GET, path, std::move(handler));
}

void HttpServer::post(const std::string& path, HttpHandler handler) {
    route(HttpMethod::POST, path, std::move(handler));
}

void HttpServer::set_default_handler(HttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    default_handler_ = std::move(handler);
}

std::shared_ptr<const HttpHandler> HttpServer::match_route(
    HttpMethod method, const std::string& path) const {
    std::lock_guard<std::mutex> lock(routes_mutex_);

    // Exact match first.
    auto it = routes_.find({method, path});
    if (it != routes_.end()) {
        return it->second;
    }
    // ANY-method match.
    it = routes_.find({HttpMethod::ANY, path});
    if (it != routes_.end()) {
        return it->second;
    }
    // ':' parameter patterns.
    const auto segments = split_path(path);
    for (const auto& [key, handler] : routes_) {
        if (key.method != method && key.method != HttpMethod::ANY) {
            continue;
        }
        if (key.path.find('/:') == std::string::npos) {
            continue;
        }
        if (pattern_matches(split_path(key.path), segments)) {
            return handler;
        }
    }
    return nullptr;
}

HttpResponse HttpServer::dispatch(const HttpRequest& req) const {
    HttpResponse response;
    response.version(req.version());
    response.set(http::field::server, "Shield/1.0");

    std::string path(req.target());
    // Strip query string for route matching.
    const auto query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }

    auto handler = match_route(verb_to_method(req.method()), path);
    if (handler) {
        response = (*handler)(req);
    } else if (default_handler_) {
        auto def = default_handler_;
        response = def(req);
    } else {
        response.result(http::status::not_found);
        response.set(http::field::content_type, "application/json");
        response.body() =
            R"({"error":"not_found","message":"route not found"})";
    }

    if (response.base().find(http::field::content_type) ==
        response.base().end()) {
        response.set(http::field::content_type, "application/json");
    }
    response.prepare_payload();
    return response;
}

void HttpServer::start() {
    if (running_) return;

    auto& log = shield::log::get_logger("http");

    try {
        auto address = net::ip::make_address(config_.host);
        acceptor_ = std::make_unique<net::ip::tcp::acceptor>(
            io_context_, net::ip::tcp::endpoint(address, config_.port));

        acceptor_->set_option(net::socket_base::reuse_address(true));

        running_ = true;
        do_accept();

        io_thread_ = std::thread([this]() { io_context_.run(); });

        SHIELD_LOG_INFO(log, "HTTP server started on " + config_.host + ":" +
                                 std::to_string(config_.port));
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR(
            log, "HTTP server failed to start: " + std::string(e.what()));
        running_ = false;
    }
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;

    boost::system::error_code ec;
    if (acceptor_) {
        acceptor_->close(ec);
    }
    io_context_.stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    io_context_.restart();

    auto& log = shield::log::get_logger("http");
    SHIELD_LOG_INFO(log, "HTTP server stopped");
}

bool HttpServer::is_running() const { return running_; }

void HttpServer::do_accept() {
    acceptor_->async_accept(
        net::make_strand(io_context_),
        [this](boost::beast::error_code ec, net::ip::tcp::socket socket) {
            if (ec) {
                if (ec != net::error::operation_aborted) {
                    auto& log = shield::log::get_logger("http");
                    SHIELD_LOG_ERROR(log, "HTTP accept error: " + ec.message());
                }
                return;
            }

            // Handle the session in a strand for thread safety.
            auto socket_ptr =
                std::make_shared<net::ip::tcp::socket>(std::move(socket));
            handle_session(socket_ptr);

            if (running_) {
                do_accept();
            }
        });
}

// Per-connection session. The shared_from_this pattern ties the socket's
// lifetime to the pending async chain: when a connection errors, reaches
// EOF, or finishes a non-keep-alive exchange, the callbacks stop
// re-arming and the last reference drops, closing the socket.
struct HttpServer::Session : std::enable_shared_from_this<HttpServer::Session> {
    HttpServer& server;
    std::shared_ptr<net::ip::tcp::socket> socket;
    std::shared_ptr<beast::flat_buffer> buffer;

    Session(HttpServer& srv, std::shared_ptr<net::ip::tcp::socket> sock)
        : server(srv),
          socket(std::move(sock)),
          buffer(std::make_shared<beast::flat_buffer>()) {}

    void start() { read_next(); }

    void read_next() {
        auto req = std::make_shared<http::request<http::string_body>>();
        http::async_read(*socket, *buffer, *req,
                         [self = shared_from_this(), req](
                             boost::beast::error_code ec, std::size_t) mutable {
                             self->on_read(ec, std::move(req));
                         });
    }

    void on_read(boost::beast::error_code ec,
                 std::shared_ptr<http::request<http::string_body>> req) {
        if (ec == http::error::end_of_stream) {
            // Client closed the connection gracefully.
            boost::system::error_code shutdown_ec;
            socket->shutdown(net::ip::tcp::socket::shutdown_send, shutdown_ec);
            return;  // dropping `self` closes the socket
        }
        if (ec) {
            boost::system::error_code close_ec;
            socket->close(close_ec);
            return;  // read error, drop connection
        }

        HttpResponse response = server.dispatch(*req);
        auto shared_response =
            std::make_shared<HttpResponse>(std::move(response));
        const bool keep =
            connection_keeps_alive(*shared_response, req->version());

        http::async_write(
            *socket, *shared_response,
            [self = shared_from_this(), shared_response, keep](
                boost::beast::error_code ec, std::size_t) mutable {
                self->on_write(ec, keep);
            });
    }

    void on_write(boost::beast::error_code ec, bool keep) {
        if (ec) {
            boost::system::error_code close_ec;
            socket->close(close_ec);
            return;  // write error, drop connection
        }
        if (!keep) {
            // HTTP/1.0 request or "Connection: close": done with this
            // connection.
            boost::system::error_code shutdown_ec;
            socket->shutdown(net::ip::tcp::socket::shutdown_send, shutdown_ec);
            return;
        }
        buffer->clear();
        read_next();
    }
};

void HttpServer::handle_session(std::shared_ptr<net::ip::tcp::socket> socket) {
    std::make_shared<Session>(*this, std::move(socket))->start();
}

}  // namespace shield::net
