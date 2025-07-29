#pragma once

#include <functional>
#include <regex>
#include <unordered_map>

#include "shield/net/session.hpp"
#include "shield/protocol/protocol_handler.hpp"

namespace shield::protocol {

// HTTP request router
class HttpRouter {
public:
    using RouteHandler = std::function<HttpResponse(const HttpRequest &)>;

    void add_route(const std::string &method, const std::string &path_pattern,
                   RouteHandler handler);
    HttpResponse route_request(const HttpRequest &request);

private:
    struct Route {
        std::string method;
        std::regex path_regex;
        RouteHandler handler;
    };

    std::vector<Route> routes_;
    RouteHandler default_handler_;
};

// HTTP protocol handler implementation
class HttpProtocolHandler : public IProtocolHandler {
public:
    HttpProtocolHandler();
    ~HttpProtocolHandler() override = default;

    void handle_data(uint64_t connection_id, const char *data,
                     size_t length) override;
    void handle_connection(uint64_t connection_id) override;
    void handle_disconnection(uint64_t connection_id) override;
    bool send_data(uint64_t connection_id, const std::string &data) override;
    ProtocolType get_protocol_type() const override {
        return ProtocolType::HTTP;
    }

    // HTTP-specific methods
    void set_session_provider(
        std::function<std::shared_ptr<net::Session>(uint64_t)> provider);
    HttpRouter &get_router() { return router_; }

private:
    HttpRequest parse_http_request(const std::string &raw_request,
                                   uint64_t connection_id);
    std::string format_http_response(const HttpResponse &response);
    bool is_complete_request(const std::string &data);

    std::unordered_map<uint64_t, std::string> connection_buffers_;
    std::function<std::shared_ptr<net::Session>(uint64_t)> session_provider_;
    HttpRouter router_;
};

// Factory function
std::unique_ptr<HttpProtocolHandler> create_http_handler();

}  // namespace shield::protocol