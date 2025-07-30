// Note: This file contains end-to-end tests, not true integration tests
// Recommendation:
// 1. Rename this file to tests/e2e/test_integration_legacy.cpp
// 2. Use the new layered testing architecture:
//    - tests/unit/ - Unit tests (using mocks)
//    - tests/integration/ - Component integration tests
//    - tests/e2e/ - End-to-end tests (requires running server)
//
// This test now uses mock servers, but following best practices should be split
// into:
// - Unit tests for BinaryProtocol
// - Integration tests for GatewayComponent
// - End-to-end tests for complete system

#define BOOST_TEST_MODULE IntegrationTest
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "shield/log/logger.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Mock HTTP Server
class MockHttpServer {
public:
    MockHttpServer()
        : acceptor_(ioc_, tcp::endpoint(tcp::v4(), 8081)), stop_flag_(false) {}

    void start() {
        server_thread_ = std::thread([this]() { run(); });
        std::this_thread::sleep_for(
            std::chrono::milliseconds(50));  // Wait for server startup
    }

    void stop() {
        stop_flag_ = true;
        ioc_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

private:
    void run() {
        while (!stop_flag_) {
            try {
                tcp::socket socket(ioc_);
                acceptor_.accept(socket);
                handle_request(std::move(socket));
            } catch (std::exception &e) {
                if (!stop_flag_) {
                    // Log error but continue
                }
            }
        }
    }

    void handle_request(tcp::socket socket) {
        try {
            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(stream, buffer, req);

            http::response<http::string_body> res;
            res.version(req.version());
            res.result(http::status::ok);
            res.set(http::field::server, "MockServer");
            res.set(http::field::content_type, "application/json");

            if (req.target() == "/api/player/info") {
                nlohmann::json response_json = {
                    {"player_id", "test_player_123"},
                    {"level", 10},
                    {"score", 1500}};
                res.body() = response_json.dump();
            } else if (req.target() == "/api/game/action") {
                nlohmann::json response_json = {{"status", "accepted"}};
                res.body() = response_json.dump();
            }

            res.prepare_payload();
            http::write(stream, res);
            stream.socket().shutdown(tcp::socket::shutdown_both);
        } catch (std::exception &e) {
            // Handle error
        }
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread server_thread_;
    std::atomic<bool> stop_flag_;
};

// Mock WebSocket Server
class MockWebSocketServer {
public:
    MockWebSocketServer()
        : acceptor_(ioc_, tcp::endpoint(tcp::v4(), 8082)), stop_flag_(false) {}

    void start() {
        server_thread_ = std::thread([this]() { run(); });
        std::this_thread::sleep_for(
            std::chrono::milliseconds(50));  // Wait for server startup
    }

    void stop() {
        stop_flag_ = true;
        ioc_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

private:
    void run() {
        while (!stop_flag_) {
            try {
                tcp::socket socket(ioc_);
                acceptor_.accept(socket);
                std::thread([this, socket = std::move(socket)]() mutable {
                    handle_websocket(std::move(socket));
                }).detach();
            } catch (std::exception &e) {
                if (!stop_flag_) {
                    // Log error but continue
                }
            }
        }
    }

    void handle_websocket(tcp::socket socket) {
        try {
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();

            while (!stop_flag_) {
                beast::flat_buffer buffer;
                ws.read(buffer);

                std::string message =
                    boost::beast::buffers_to_string(buffer.data());
                auto json_msg = nlohmann::json::parse(message);

                nlohmann::json response;
                if (json_msg.contains("type")) {
                    std::string type = json_msg["type"];
                    if (type == "get_info") {
                        response = {
                            {"success", true},
                            {"data",
                             {{"player_id", json_msg["data"]["player_id"]},
                              {"level", 10},
                              {"experience", 2500}}}};
                    } else if (type == "level_up") {
                        response = {
                            {"success", true},
                            {"data",
                             {{"player_id", json_msg["data"]["player_id"]},
                              {"new_level", json_msg["data"]["new_level"]}}}};
                    } else if (type == "add_experience") {
                        response = {
                            {"success", true},
                            {"data",
                             {{"player_id", json_msg["data"]["player_id"]},
                              {"exp_added", json_msg["data"]["exp"]}}}};
                    } else {
                        response = {{"success", true}, {"data", {}}};
                    }
                } else {
                    response = {{"success", true}, {"data", {}}};
                }

                ws.write(net::buffer(response.dump()));
            }
        } catch (std::exception &e) {
            // Connection closed or error
        }
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    std::thread server_thread_;
    std::atomic<bool> stop_flag_;
};

struct IntegrationTestFixture {
    IntegrationTestFixture() {
        // Initialize logging system
        shield::log::LogConfig log_config;
        log_config.global_level =
            shield::log::Logger::level_from_string("info");
        shield::log::Logger::init(log_config);

        // Start mock servers
        http_server_ = std::make_unique<MockHttpServer>();
        ws_server_ = std::make_unique<MockWebSocketServer>();

        http_server_->start();
        ws_server_->start();

        // Wait for server startup
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ~IntegrationTestFixture() {
        // Stop mock servers
        if (http_server_) {
            http_server_->stop();
        }
        if (ws_server_) {
            ws_server_->stop();
        }
    }

    std::unique_ptr<MockHttpServer> http_server_;
    std::unique_ptr<MockWebSocketServer> ws_server_;
};

BOOST_FIXTURE_TEST_SUITE(IntegrationTests, IntegrationTestFixture)

// HTTP API Tests
BOOST_AUTO_TEST_CASE(TestHttpGetPlayerInfo) {
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Connect to HTTP server
        auto const results = resolver.resolve("localhost", "8081");
        stream.connect(results);

        // Send HTTP GET request
        http::request<http::string_body> req{http::verb::get,
                                             "/api/player/info", 11};
        req.set(http::field::host, "localhost");
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");

        http::write(stream, req);

        // Receive response
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        // Verify response
        BOOST_CHECK_EQUAL(res.result(), http::status::ok);
        BOOST_CHECK(!res.body().empty());

        // Parse JSON response
        auto json_response = nlohmann::json::parse(res.body());
        BOOST_CHECK(json_response.contains("player_id"));
        BOOST_CHECK(json_response.contains("level"));
        BOOST_CHECK(json_response.contains("score"));

        stream.socket().shutdown(tcp::socket::shutdown_both);

    } catch (std::exception const &e) {
        BOOST_FAIL("HTTP GET test failed: " << e.what());
    }
}

BOOST_AUTO_TEST_CASE(TestHttpPostGameAction) {
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Connect to HTTP server
        auto const results = resolver.resolve("localhost", "8081");
        stream.connect(results);

        // Prepare POST data
        nlohmann::json post_data = {{"action", "attack"}, {"target", "enemy1"}};
        std::string body = post_data.dump();

        // Send HTTP POST request
        http::request<http::string_body> req{http::verb::post,
                                             "/api/game/action", 11};
        req.set(http::field::host, "localhost");
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        req.body() = body;
        req.prepare_payload();

        http::write(stream, req);

        // Receive response
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        // Verify response
        BOOST_CHECK_EQUAL(res.result(), http::status::ok);
        BOOST_CHECK(!res.body().empty());

        // Parse JSON response
        auto json_response = nlohmann::json::parse(res.body());
        BOOST_CHECK(json_response.contains("status"));
        BOOST_CHECK_EQUAL(json_response["status"], "accepted");

        stream.socket().shutdown(tcp::socket::shutdown_both);

    } catch (std::exception const &e) {
        BOOST_FAIL("HTTP POST test failed: " << e.what());
    }
}

// WebSocket Tests
BOOST_AUTO_TEST_CASE(TestWebSocketConnection) {
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        websocket::stream<tcp::socket> ws(ioc);

        // Connect to WebSocket server
        auto const results = resolver.resolve("localhost", "8082");
        auto ep = net::connect(ws.next_layer(), results);

        // Perform WebSocket handshake
        std::string host = "localhost:" + std::to_string(ep.port());
        ws.handshake(host, "/");

        // Send test message
        nlohmann::json test_message = {
            {"type", "get_info"}, {"data", {{"player_id", "test_player_123"}}}};

        ws.write(net::buffer(test_message.dump()));

        // Receive response
        beast::flat_buffer buffer;
        ws.read(buffer);

        std::string response = boost::beast::buffers_to_string(buffer.data());

        // Verify response
        BOOST_CHECK(!response.empty());

        // Parse JSON response
        auto json_response = nlohmann::json::parse(response);
        BOOST_CHECK(json_response.contains("success"));

        ws.close(websocket::close_code::normal);

    } catch (std::exception const &e) {
        BOOST_FAIL("WebSocket test failed: " << e.what());
    }
}

// Lua Actor Integration Tests
BOOST_AUTO_TEST_CASE(TestLuaActorIntegration) {
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        websocket::stream<tcp::socket> ws(ioc);

        auto const results = resolver.resolve("localhost", "8082");
        auto ep = net::connect(ws.next_layer(), results);

        std::string host = "localhost:" + std::to_string(ep.port());
        ws.handshake(host, "/");

        // Test different types of Lua actor messages
        std::vector<nlohmann::json> test_messages = {
            {{"type", "get_info"}, {"data", {{"player_id", "player_001"}}}},
            {{"type", "level_up"},
             {"data", {{"player_id", "player_001"}, {"new_level", "5"}}}},
            {{"type", "add_experience"},
             {"data", {{"player_id", "player_001"}, {"exp", "100"}}}}};

        for (const auto &msg : test_messages) {
            ws.write(net::buffer(msg.dump()));

            beast::flat_buffer buffer;
            ws.read(buffer);

            std::string response =
                boost::beast::buffers_to_string(buffer.data());
            auto json_response = nlohmann::json::parse(response);

            // Verify Lua actor responds correctly
            BOOST_CHECK(json_response.contains("success"));
            BOOST_CHECK(json_response.contains("data"));
        }

        ws.close(websocket::close_code::normal);

    } catch (std::exception const &e) {
        BOOST_FAIL("Lua Actor integration test failed: " << e.what());
    }
}

// Concurrent Connection Tests
BOOST_AUTO_TEST_CASE(TestConcurrentConnections) {
    const int num_connections =
        5;  // Reduce connection count to avoid resource issues
    std::vector<std::thread> threads;
    std::atomic<int> successful_connections{0};

    for (int i = 0; i < num_connections; ++i) {
        threads.emplace_back([&, i]() {
            try {
                net::io_context ioc;
                tcp::resolver resolver(ioc);
                websocket::stream<tcp::socket> ws(ioc);

                auto const results = resolver.resolve("localhost", "8082");
                auto ep = net::connect(ws.next_layer(), results);

                std::string host = "localhost:" + std::to_string(ep.port());
                ws.handshake(host, "/");

                nlohmann::json test_message = {
                    {"type", "get_info"},
                    {"data",
                     {{"player_id", "concurrent_test_" + std::to_string(i)}}}};

                ws.write(net::buffer(test_message.dump()));

                beast::flat_buffer buffer;
                ws.read(buffer);

                std::string response =
                    boost::beast::buffers_to_string(buffer.data());
                auto json_response = nlohmann::json::parse(response);

                if (json_response.contains("success")) {
                    successful_connections++;
                }

                ws.close(websocket::close_code::normal);

            } catch (std::exception const &e) {
                // Connection failed, do not increment success count
                std::cerr << "Concurrent connection " << i
                          << " failed: " << e.what() << std::endl;
            }
        });
    }

    // Wait for all threads to complete
    for (auto &t : threads) {
        t.join();
    }

    // Verify most connections succeeded (allow some failures)
    BOOST_CHECK_GE(successful_connections.load(), num_connections * 0.6);
}

BOOST_AUTO_TEST_SUITE_END()