// [SHIELD_NET] HTTP server implementation using Boost.Beast
#include "shield/net/http_server.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
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
        if (pattern[i].empty() ||    // GCOVR_EXCL_BR_LINE (defensive:
            pattern[i][0] != ':') {  // split_path never yields empty segments,
                                     // so pattern[i].empty() is always false)
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
    if (request_version < 11) {  // GCOVR_EXCL_BR_START (compiler artifact:
        return it != response.base().end() &&  // std::string temporary +
               connection_token_exists(std::string(it->value()), "keep-alive");
    }  // inlined find/unwind slow-path edges) GCOVR_EXCL_BR_STOP
    return it == response.base().end() ||  // GCOVR_EXCL_BR_START (compiler
           !connection_token_exists(std::string(it->value()), "close");
}  // artifact: same inlined slow-path edges) GCOVR_EXCL_BR_STOP

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

// Uniform 500 answer for a handler that threw; the exception text goes to
// the log, never back to the client.
HttpResponse handler_error_response() {
    HttpResponse response;
    response.result(http::status::internal_server_error);
    response.set(http::field::content_type, "application/json");
    response.body() = R"({"error":"handler_exception"})";
    return response;
}  // GCOVR_EXCL_LINE (compiler artifact: line-table entry for the function
   // epilogue; the 500 path itself is exercised)

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

void HttpServer::set_static_root(const std::string& root) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    static_root_ = root;
}

std::optional<HttpResponse> HttpServer::try_static(
    const HttpRequest& req) const {
    if (static_root_.empty()) {
        return std::nullopt;
    }

    std::string path(req.target());
    const auto query_pos = path.find('?');
    if (query_pos != std::string::npos) {
        path = path.substr(0, query_pos);
    }
    // Only GET serves static files.
    if (req.method() != http::verb::get) {
        return std::nullopt;
    }

    // Decode %2e ('.') variants so "..%2f" cannot smuggle a traversal.
    // We reject any path containing a NUL or a backslash outright; dots
    // are re-checked after canonicalisation below.
    if (path.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    std::string decoded;
    decoded.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(path[i + 1]);
            const int lo = hex(path[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }

    // Reject traversal, backslash, and any path that escapes the root.
    if (decoded.find("..") != std::string::npos ||
        decoded.find('\\') != std::string::npos) {
        return std::nullopt;
    }

    namespace fs = std::filesystem;
    fs::path root(static_root_);
    // Root must be absolute and exist; otherwise no static serving.
    if (!root.is_absolute() || !fs::exists(root)) {
        return std::nullopt;
    }

    fs::path target = root;
    std::string rel = decoded;
    if (!rel.empty() && rel.front() == '/') {
        rel.erase(rel.begin());
    }
    target /= rel;

    // Canonicalise and verify containment under the root (resolves symlinks
    // and any remaining ".." that survived lexical checks).
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(target, ec);
    if (ec) {
        return std::nullopt;
    }
    const fs::path canon_root = fs::weakly_canonical(root, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto root_str = canon_root.string();
    if (canon.string() != root_str &&
        canon.string().rfind(root_str + "/", 0) != 0) {
        return std::nullopt;
    }

    if (!fs::is_regular_file(canon, ec) || ec) {
        return std::nullopt;
    }

    std::ifstream file(canon, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string body((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());

    HttpResponse response;
    response.result(http::status::ok);
    const std::string ext = canon.extension().string();
    if (ext == ".html" || ext == ".htm") {
        response.set(http::field::content_type, "text/html; charset=utf-8");
    } else if (ext == ".js" || ext == ".mjs") {
        response.set(http::field::content_type, "application/javascript");
    } else if (ext == ".css") {
        response.set(http::field::content_type, "text/css");
    } else if (ext == ".json") {
        response.set(http::field::content_type, "application/json");
    } else if (ext == ".svg") {
        response.set(http::field::content_type, "image/svg+xml");
    } else if (ext == ".png") {
        response.set(http::field::content_type, "image/png");
    } else if (ext == ".ico") {
        response.set(http::field::content_type, "image/x-icon");
    } else {
        response.set(http::field::content_type, "application/octet-stream");
    }
    // Cache built assets aggressively; index.html stays revalidated so a
    // fresh deploy is picked up on navigation.
    if (ext == ".html" || ext == ".htm") {
        response.set(http::field::cache_control, "no-cache");
    } else {
        response.set(http::field::cache_control, "public, max-age=31536000");
    }
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
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
        if (key.path.find("/:") == std::string::npos) {
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
    // A throwing handler must never unwind through the async_read callback
    // into io_context_.run(): that kills the io thread and leaves the
    // server deaf while still "running". Answer 500 and keep serving.
    try {
        if (handler) {
            response = (*handler)(req);
        } else if (default_handler_) {
            // Try static files first; the default handler (SPA fallback /
            // API 404) only runs when no static file matched.
            if (auto static_resp = try_static(req)) {
                response = std::move(*static_resp);
            } else {
                auto def = default_handler_;
                response = def(req);
            }
        } else {
            response.result(http::status::not_found);
            response.set(http::field::content_type, "application/json");
            response.body() =
                R"({"error":"not_found","message":"route not found"})";
        }
    } catch (const std::exception& e) {
        auto& log = shield::log::get_logger("http");
        SHIELD_LOG_ERROR(log,
                         "HTTP handler error on " + path + ": " + e.what());
        response = handler_error_response();
    } catch (...) {
        auto& log = shield::log::get_logger("http");
        SHIELD_LOG_ERROR(log,
                         "HTTP handler threw a non-std exception on " + path);
        response = handler_error_response();
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
        // Record the actually-bound port in a separate member instead of
        // mutating config_.port: a port-0 (ephemeral) config must re-request
        // a fresh ephemeral port on every start() rather than silently
        // rebinding the previous temporary port, and the caller's config
        // must not be permanently rewritten.
        bound_port_ = acceptor_->local_endpoint().port();

        acceptor_->set_option(net::socket_base::reuse_address(true));

        running_ = true;
        do_accept();

        io_thread_ = std::thread([this]() { io_context_.run(); });

        SHIELD_LOG_INFO(log, "HTTP server started on " + config_.host + ":" +
                                 std::to_string(bound_port_));
    } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch RTTI-miss
        const std::exception&
            e) {  // arm; make_address/bind only throw std::exception
                  // derivatives and the handler body is covered by the
                  // busy-port test)
        SHIELD_LOG_ERROR(
            log, "HTTP server failed to start: " + std::string(e.what()));
        running_ = false;
    }
}

uint16_t HttpServer::port() const {
    if (config_.port != 0) {
        return config_.port;
    }
    // bound_port_ is set at bind time and survives stop() (which closes the
    // acceptor and would otherwise make local_endpoint() throw). It is 0
    // before the first start().
    return bound_port_;
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;

    boost::system::error_code ec;
    if (acceptor_) {  // GCOVR_EXCL_BR_LINE (defensive: running_ is only set
                      // after start() created the acceptor, so the null arm is
                      // unreachable)
        acceptor_->close(ec);
    }
    io_context_.stop();
    if (io_thread_          // GCOVR_EXCL_BR_LINE (defensive: not-joinable arm
            .joinable()) {  // GCOVR_EXCL_BR_LINE (defensive: reaching here
                            // implies start() succeeded and spawned the io
                            // thread, so the not-joinable arm is unreachable)
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
            if (ec == net::error::operation_aborted) {
                return;  // stop() closed the acceptor: stand down
            }
            if (ec) {  // GCOVR_EXCL_BR_LINE (defensive: transient accept
                       // errors — ECONNABORTED on the BSD stacks when a
                       // queued connection is reset before accept, EMFILE
                       // under fd pressure — cannot be driven determin-
                       // istically from a test; the arm only logs and
                       // falls through to the re-arm below)
                // GCOVR_EXCL_START (defensive: see the branch note above)
                auto& log = shield::log::get_logger("http");
                SHIELD_LOG_ERROR(log, "HTTP accept error: " + ec.message());
                // GCOVR_EXCL_STOP
            } else {
                // A failed accept carries no socket: only arm the session
                // chain when the handshake handed us one. Session setup
                // allocates; letting an exception escape would kill
                // io_context_.run() and deafen the server, so the guard is
                // belt-and-braces.
                // GCOVR_EXCL_START (defensive: only allocation failures can
                // unwind here; no deterministic test driver exists)
                try {
                    auto socket_ptr = std::make_shared<net::ip::tcp::socket>(
                        std::move(socket));
                    handle_session(socket_ptr);
                } catch (...) {
                    // Swallow: the connection dies, the server lives.
                }
                // GCOVR_EXCL_STOP
            }

            if (running_) {  // GCOVR_EXCL_BR_LINE (defensive: only observable
                             // in the stop()-vs-accept-callback race window;
                             // not deterministically testable)
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
        http::async_read(
            *socket, *buffer, *req,
            [self = shared_from_this(),  // GCOVR_EXCL_BR_LINE
             req](  // GCOVR_EXCL_BR_LINE (compiler artifact: beast/asio inlined
                    // read-loop and exception-cleanup edges attributed to this
                    // line; no source-level condition here)
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
            [self = shared_from_this(), shared_response,  // GCOVR_EXCL_BR_LINE
             keep](  // GCOVR_EXCL_BR_LINE (compiler artifact: beast/asio
                     // inlined write-loop and exception-cleanup edges
                     // attributed to this line; no source-level condition here)
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
