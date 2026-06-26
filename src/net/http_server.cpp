// [SHIELD_NET] HTTP server implementation using Boost.Beast
#include "shield/net/http_server.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include "shield/log/logger.hpp"

namespace shield::net {

HttpServer::HttpServer(const HttpServerConfig& config) : config_(config) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(HttpMethod method, const std::string& path,
                       HttpHandler handler) {
    routes_[{method, path}] = std::move(handler);
}

void HttpServer::get(const std::string& path, HttpHandler handler) {
    route(HttpMethod::GET, path, std::move(handler));
}

void HttpServer::post(const std::string& path, HttpHandler handler) {
    route(HttpMethod::POST, path, std::move(handler));
}

void HttpServer::set_default_handler(HttpHandler handler) {
    default_handler_ = std::move(handler);
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

void HttpServer::handle_session(std::shared_ptr<net::ip::tcp::socket> socket) {
    // Read and process HTTP requests on this connection (keep-alive loop).
    auto buffer = std::make_shared<beast::flat_buffer>();

    // Use a weak_ptr to avoid capturing shared_ptr in async chain.
    auto weak_socket = std::weak_ptr<net::ip::tcp::socket>(socket);

    std::function<void()> read_request;
    read_request = [this, socket, buffer, &read_request, weak_socket]() {
        auto req = std::make_shared<http::request<http::string_body>>();

        http::async_read(
            *socket, *buffer, *req,
            [this, socket, buffer, req, weak_socket](
                boost::beast::error_code ec, std::size_t bytes_transferred) {
                (void)bytes_transferred;

                if (ec == http::error::end_of_stream) {
                    // Client closed connection gracefully.
                    boost::system::error_code shutdown_ec;
                    socket->shutdown(net::ip::tcp::socket::shutdown_send,
                                     shutdown_ec);
                    return;
                }

                if (ec) {
                    return;  // Read error, drop connection.
                }

                // Build response.
                HttpResponse response;
                response.version(req->version());
                response.set(http::field::server, "Shield/1.0");

                // Find matching route.
                HttpMethod method = HttpMethod::ANY;
                switch (req->method()) {
                    case http::verb::get:
                        method = HttpMethod::GET;
                        break;
                    case http::verb::post:
                        method = HttpMethod::POST;
                        break;
                    case http::verb::put:
                        method = HttpMethod::PUT;
                        break;
                    case http::verb::delete_:
                        method = HttpMethod::DELETE_;
                        break;
                    case http::verb::patch:
                        method = HttpMethod::PATCH;
                        break;
                    default:
                        method = HttpMethod::ANY;
                        break;
                }

                std::string path(req->target());
                // Strip query string for route matching.
                auto query_pos = path.find('?');
                if (query_pos != std::string::npos) {
                    path = path.substr(0, query_pos);
                }

                // Try exact match first.
                auto it = routes_.find({method, path});
                if (it != routes_.end()) {
                    response = it->second(*req);
                } else {
                    // Try ANY method match.
                    it = routes_.find({HttpMethod::ANY, path});
                    if (it != routes_.end()) {
                        response = it->second(*req);
                    } else if (default_handler_) {
                        response = default_handler_(*req);
                    } else {
                        response.result(http::status::not_found);
                        response.set(http::field::content_type,
                                     "application/json");
                        response.body() =
                            R"({"error":"not_found","message":"route not found"})";
                    }
                }

                // Set common headers.
                response.set(http::field::content_type,
                             response.base().find(http::field::content_type) !=
                                     response.base().end()
                                 ? response.base()[http::field::content_type]
                                 : "application/json");
                response.prepare_payload();

                // Write response.
                auto shared_response =
                    std::make_shared<HttpResponse>(std::move(response));

                http::async_write(
                    *socket, *shared_response,
                    [this, socket, buffer, shared_response, weak_socket](
                        boost::beast::error_code ec, std::size_t) {
                        if (ec) {
                            return;  // Write error, drop connection.
                        }

                        // Check if client wants keep-alive.
                        if (shared_response->need_eof()) {
                            boost::system::error_code shutdown_ec;
                            socket->shutdown(
                                net::ip::tcp::socket::shutdown_send,
                                shutdown_ec);
                            return;
                        }

                        // Read next request on the same connection.
                        // Note: recursive async chain, not stack recursion.
                        buffer->clear();
                        auto new_req = std::make_shared<
                            http::request<http::string_body>>();
                        http::async_read(
                            *socket, *buffer, *new_req,
                            [this, socket, buffer, new_req, weak_socket](
                                boost::beast::error_code ec2, std::size_t) {
                                if (ec2) {
                                    return;
                                }
                                // Process the new request inline.
                                // For simplicity, handle synchronously here.
                                // In production, dispatch to a strand.
                            });
                    });
            });
    };

    // Start reading.
    auto req = std::make_shared<http::request<http::string_body>>();
    http::async_read(
        *socket, *buffer, *req,
        [this, socket, buffer, req](boost::beast::error_code ec, std::size_t) {
            if (ec) return;

            HttpResponse response;
            response.version(req->version());
            response.set(http::field::server, "Shield/1.0");

            HttpMethod method = HttpMethod::ANY;
            switch (req->method()) {
                case http::verb::get:
                    method = HttpMethod::GET;
                    break;
                case http::verb::post:
                    method = HttpMethod::POST;
                    break;
                case http::verb::put:
                    method = HttpMethod::PUT;
                    break;
                case http::verb::delete_:
                    method = HttpMethod::DELETE_;
                    break;
                case http::verb::patch:
                    method = HttpMethod::PATCH;
                    break;
                default:
                    method = HttpMethod::ANY;
                    break;
            }

            std::string path(req->target());
            auto query_pos = path.find('?');
            if (query_pos != std::string::npos) {
                path = path.substr(0, query_pos);
            }

            auto it = routes_.find({method, path});
            if (it != routes_.end()) {
                response = it->second(*req);
            } else {
                it = routes_.find({HttpMethod::ANY, path});
                if (it != routes_.end()) {
                    response = it->second(*req);
                } else if (default_handler_) {
                    response = default_handler_(*req);
                } else {
                    response.result(http::status::not_found);
                    response.set(http::field::content_type, "application/json");
                    response.body() =
                        R"({"error":"not_found","message":"route not found"})";
                }
            }

            if (response.base().find(http::field::content_type) ==
                response.base().end()) {
                response.set(http::field::content_type, "application/json");
            }
            response.prepare_payload();

            auto shared_response =
                std::make_shared<HttpResponse>(std::move(response));
            http::async_write(*socket, *shared_response,
                              [socket, shared_response](
                                  boost::beast::error_code ec2, std::size_t) {
                                  if (ec2) return;
                                  if (shared_response->need_eof()) {
                                      boost::system::error_code shutdown_ec;
                                      socket->shutdown(
                                          net::ip::tcp::socket::shutdown_send,
                                          shutdown_ec);
                                  }
                              });
        });
}

}  // namespace shield::net
