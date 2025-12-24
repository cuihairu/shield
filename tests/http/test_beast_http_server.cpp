// tests/http/test_beast_http_server.cpp
#define BOOST_TEST_MODULE BeastHttpServerTests
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "shield/http/beast_http_server.hpp"
#include "shield/protocol/protocol_handler.hpp"

namespace shield::http {
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Test fixture for HTTP server
class HttpServerFixture {
public:
    HttpServerFixture() : port_(18082) {
        // Simple request handler that responds with request info
        handler_ = [](const protocol::HttpRequest& req) -> protocol::HttpResponse {
            protocol::HttpResponse res;
            res.status_code = 200;
            res.status_text = "OK";

            if (req.path == "/test") {
                res.body = R"({"message":"test endpoint","path":")" + req.path + R"("})";
            } else if (req.path == "/echo") {
                res.body = req.body;
            } else if (req.path == "/headers") {
                std::string headers_json = "{";
                bool first = true;
                for (const auto& [k, v] : req.headers) {
                    if (!first) headers_json += ",";
                    headers_json += "\"" + k + "\":\"" + v + "\"";
                    first = false;
                }
                headers_json += "}";
                res.body = headers_json;
            } else if (req.path == "/method") {
                res.body = R"({"method":")" + req.method + R"("})";
            } else if (req.path == "/error") {
                res.status_code = 500;
                res.status_text = "Internal Server Error";
                res.body = R"({"error":"test error"})";
            } else if (req.path == "/not_found") {
                res.status_code = 404;
                res.status_text = "Not Found";
                res.body = R"({"error":"not found"})";
            } else {
                res.body = R"({"message":"Hello, Shield!"})";
            }

            return res;
        };

        BeastHttpServerConfig config;
        config.host = "127.0.0.1";
        config.port = port_;
        config.threads = 2;
        config.max_request_size = 1024 * 1024;

        server_ = std::make_unique<BeastHttpServer>(config, handler_);
    }

    ~HttpServerFixture() {
        if (server_ && server_->is_running()) {
            server_->stop();
        }
    }

    void start_server() {
        server_->start();
        // Give server time to start listening
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void stop_server() { server_->stop(); }

    bool is_server_running() const { return server_->is_running(); }

    uint16_t get_port() const { return port_; }

    // Helper method to make HTTP requests
    template <http::verb method, class Body, class Allocator>
    auto make_request(const std::string& target,
                      const Body& body,
                      const Allocator& allocator = std::allocator<char>())
        -> http::response<http::string_body> {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        auto const results = resolver.resolve("127.0.0.1", std::to_string(port_));
        stream.connect(results);

        http::request<http::string_body> req(method, target, 11);
        req.set(http::field::host, "127.0.0.1:" + std::to_string(port_));
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        return res;
    }

private:
    uint16_t port_;
    BeastHttpServer::RequestHandler handler_;
    std::unique_ptr<BeastHttpServer> server_;
};

// Configuration tests
BOOST_AUTO_TEST_SUITE(BeastHttpServerConfigTests)

BOOST_AUTO_TEST_CASE(test_default_config) {
    BeastHttpServerConfig config;

    BOOST_CHECK_EQUAL(config.host, "0.0.0.0");
    BOOST_CHECK_EQUAL(config.port, 8082);
    BOOST_CHECK_EQUAL(config.threads, 0);  // 0 means hardware concurrency
    BOOST_CHECK_EQUAL(config.root_path, "/");
    BOOST_CHECK_EQUAL(config.max_request_size, 1024 * 1024);  // 1MB
}

BOOST_AUTO_TEST_CASE(test_custom_config) {
    BeastHttpServerConfig config;
    config.host = "127.0.0.1";
    config.port = 9090;
    config.threads = 4;
    config.root_path = "/api";
    config.max_request_size = 2 * 1024 * 1024;

    BOOST_CHECK_EQUAL(config.host, "127.0.0.1");
    BOOST_CHECK_EQUAL(config.port, 9090);
    BOOST_CHECK_EQUAL(config.threads, 4);
    BOOST_CHECK_EQUAL(config.root_path, "/api");
    BOOST_CHECK_EQUAL(config.max_request_size, 2 * 1024 * 1024);
}

BOOST_AUTO_TEST_SUITE_END()

// Server lifecycle tests
BOOST_AUTO_TEST_SUITE(BeastHttpServerLifecycleTests, *boost::unit_test::fixture<HttpServerFixture>())

BOOST_AUTO_TEST_CASE(test_server_start_stop) {
    BOOST_CHECK(!is_server_running());

    start_server();
    BOOST_CHECK(is_server_running());

    stop_server();
    BOOST_CHECK(!is_server_running());
}

BOOST_AUTO_TEST_CASE(test_multiple_start_stop) {
    start_server();
    BOOST_CHECK(is_server_running());

    // Second start should be idempotent
    start_server();
    BOOST_CHECK(is_server_running());

    stop_server();
    BOOST_CHECK(!is_server_running());

    // Second stop should be idempotent
    stop_server();
    BOOST_CHECK(!is_server_running());
}

BOOST_AUTO_TEST_CASE(test_restart_server) {
    start_server();
    BOOST_CHECK(is_server_running());

    stop_server();
    BOOST_CHECK(!is_server_running());

    start_server();
    BOOST_CHECK(is_server_running());

    stop_server();
    BOOST_CHECK(!is_server_running());
}

BOOST_AUTO_TEST_SUITE_END()

// HTTP request tests
BOOST_AUTO_TEST_SUITE(BeastHttpRequestTests, *boost::unit_test::fixture<HttpServerFixture>())

BOOST_AUTO_TEST_CASE(test_get_request) {
    start_server();

    auto response = make_request<http::verb::get>("/test", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK_EQUAL(response[http::field::content_type], "application/json");
    BOOST_CHECK(response.body().find("test endpoint") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_get_root) {
    start_server();

    auto response = make_request<http::verb::get>("/", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK(response.body().find("Hello, Shield!") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_post_request) {
    start_server();

    std::string body = R"({"test":"data"})";
    auto response = make_request<http::verb::post>("/echo", body);

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK_EQUAL(response.body(), body);
}

BOOST_AUTO_TEST_CASE(test_put_request) {
    start_server();

    std::string body = R"({"updated":"data"})";
    auto response = make_request<http::verb::put>("/echo", body);

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK_EQUAL(response.body(), body);
}

BOOST_AUTO_TEST_CASE(test_delete_request) {
    start_server();

    auto response = make_request<http::verb::delete_>("/method", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK(response.body().find("DELETE") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_custom_headers) {
    start_server();

    // Note: Our test fixture doesn't support custom headers in make_request,
    // but we can test that headers are captured
    auto response = make_request<http::verb::get>("/headers", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    // Response should contain headers in JSON format
    BOOST_CHECK(response.body().find("\"host\"") != std::string::npos);
    BOOST_CHECK(response.body().find("\"user-agent\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_request_with_body) {
    start_server();

    std::string body = R"({"name":"test","value":123})";
    auto response = make_request<http::verb::post>("/echo", body);

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK_EQUAL(response.body(), body);
}

BOOST_AUTO_TEST_SUITE_END()

// HTTP response tests
BOOST_AUTO_TEST_SUITE(BeastHttpResponseTests, *boost::unit_test::fixture<HttpServerFixture>())

BOOST_AUTO_TEST_CASE(test_ok_response) {
    start_server();

    auto response = make_request<http::verb::get>("/test", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK_EQUAL(response.reason(), "OK");
}

BOOST_AUTO_TEST_CASE(test_not_found_response) {
    start_server();

    auto response = make_request<http::verb::get>("/not_found", "");

    BOOST_CHECK_EQUAL(response.result_int(), 404);
    BOOST_CHECK_EQUAL(response.reason(), "Not Found");
    BOOST_CHECK(response.body().find("not found") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_error_response) {
    start_server();

    auto response = make_request<http::verb::get>("/error", "");

    BOOST_CHECK_EQUAL(response.result_int(), 500);
    BOOST_CHECK_EQUAL(response.reason(), "Internal Server Error");
    BOOST_CHECK(response.body().find("test error") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_response_headers) {
    start_server();

    auto response = make_request<http::verb::get>("/test", "");

    BOOST_CHECK(response.find(http::field::server) != response.end());
    BOOST_CHECK_EQUAL(response[http::field::server], "shield");
    BOOST_CHECK(response.find(http::field::content_type) != response.end());
}

BOOST_AUTO_TEST_SUITE_END()

// Protocol handler tests
BOOST_AUTO_TEST_SUITE(ProtocolHandlerTests)

BOOST_AUTO_TEST_CASE(test_http_request_structure) {
    protocol::HttpRequest req;
    req.method = "GET";
    req.path = "/test";
    req.version = "HTTP/1.1";
    req.headers["Content-Type"] = "application/json";
    req.headers["User-Agent"] = "TestClient";
    req.body = R"({"test":"data"})";
    req.connection_id = 12345;

    BOOST_CHECK_EQUAL(req.method, "GET");
    BOOST_CHECK_EQUAL(req.path, "/test");
    BOOST_CHECK_EQUAL(req.version, "HTTP/1.1");
    BOOST_CHECK_EQUAL(req.headers["Content-Type"], "application/json");
    BOOST_CHECK_EQUAL(req.headers["User-Agent"], "TestClient");
    BOOST_CHECK_EQUAL(req.body, R"({"test":"data"})");
    BOOST_CHECK_EQUAL(req.connection_id, 12345);
}

BOOST_AUTO_TEST_CASE(test_http_response_structure) {
    protocol::HttpResponse res;
    res.status_code = 200;
    res.status_text = "OK";
    res.headers["Content-Type"] = "application/json";
    res.body = R"({"message":"success"})";

    BOOST_CHECK_EQUAL(res.status_code, 200);
    BOOST_CHECK_EQUAL(res.status_text, "OK");
    BOOST_CHECK_EQUAL(res.headers["Content-Type"], "application/json");
    BOOST_CHECK_EQUAL(res.body, R"({"message":"success"})");
}

BOOST_AUTO_TEST_CASE(test_http_response_default_content_type) {
    protocol::HttpResponse res;

    BOOST_CHECK_EQUAL(res.headers["Content-Type"], "application/json");
}

BOOST_AUTO_TEST_CASE(test_http_response_custom_status) {
    protocol::HttpResponse res;
    res.status_code = 404;
    res.status_text = "Not Found";
    res.body = R"({"error":"Resource not found"})";

    BOOST_CHECK_EQUAL(res.status_code, 404);
    BOOST_CHECK_EQUAL(res.status_text, "Not Found");
    BOOST_CHECK(res.body.find("Resource not found") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// Concurrent request tests
BOOST_AUTO_TEST_SUITE(ConcurrentRequestTests, *boost::unit_test::fixture<HttpServerFixture>())

BOOST_AUTO_TEST_CASE(test_multiple_sequential_requests) {
    start_server();

    for (int i = 0; i < 10; ++i) {
        auto response = make_request<http::verb::get>("/test", "");
        BOOST_CHECK_EQUAL(response.result_int(), 200);
    }
}

BOOST_AUTO_TEST_CASE(test_multiple_concurrent_requests) {
    start_server();

    auto make_client_request = [this]() {
        for (int i = 0; i < 5; ++i) {
            auto response = make_request<http::verb::get>("/test", "");
            BOOST_CHECK_EQUAL(response.result_int(), 200);
        }
    };

    std::thread t1(make_client_request);
    std::thread t2(make_client_request);
    std::thread t3(make_client_request);

    t1.join();
    t2.join();
    t3.join();
}

BOOST_AUTO_TEST_SUITE_END()

// Edge case tests
BOOST_AUTO_TEST_SUITE(EdgeCaseTests, *boost::unit_test::fixture<HttpServerFixture>())

BOOST_AUTO_TEST_CASE(test_empty_path) {
    start_server();

    auto response = make_request<http::verb::get>("", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
}

BOOST_AUTO_TEST_CASE(test_empty_body) {
    start_server();

    auto response = make_request<http::verb::post>("/echo", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK_EQUAL(response.body(), "");
}

BOOST_AUTO_TEST_CASE(test_large_body) {
    start_server();

    std::string large_body(1024 * 10, 'X');  // 10KB
    auto response = make_request<http::verb::post>("/echo", large_body);

    BOOST_CHECK_EQUAL(response.result_int(), 200);
    BOOST_CHECK_EQUAL(response.body().size(), large_body.size());
}

BOOST_AUTO_TEST_CASE(test_special_characters_in_path) {
    start_server();

    auto response = make_request<http::verb::get>("/test/path?param=value&other=123", "");

    BOOST_CHECK_EQUAL(response.result_int(), 200);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace shield::http
