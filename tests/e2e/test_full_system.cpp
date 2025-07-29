#define BOOST_TEST_MODULE EndToEndTest
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>
#include <memory>
#include <fstream>

#include "shield/core/logger.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct EndToEndTestFixture {
    EndToEndTestFixture() {
        // Initialize logger
        shield::core::LogConfig log_config;
        log_config.level = shield::core::Logger::level_from_string("info");
        shield::core::Logger::init(log_config);
        
        // Check if server is running (this requires manual setup)
        server_running = check_server_availability();
    }
    
    bool check_server_availability() {
        try {
            net::io_context ioc;
            tcp::resolver resolver(ioc);
            beast::tcp_stream stream(ioc);
            
            auto const results = resolver.resolve("localhost", "8081");
            stream.connect(results);
            stream.socket().shutdown(tcp::socket::shutdown_both);
            return true;
        } catch (...) {
            return false;
        }
    }
    
    bool server_running;
};

BOOST_FIXTURE_TEST_SUITE(EndToEndTests, EndToEndTestFixture)

BOOST_AUTO_TEST_CASE(TestFullSystemHttpFlow) {
    if (!server_running) {
        BOOST_TEST_MESSAGE("Server not running, skipping end-to-end test. "
                          "Start shield server manually to run this test.");
        return;
    }
    
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        
        auto const results = resolver.resolve("localhost", "8081");
        stream.connect(results);
        
        http::request<http::string_body> req{http::verb::get, "/api/player/info", 11};
        req.set(http::field::host, "localhost");
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");
        
        http::write(stream, req);
        
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        
        BOOST_CHECK_EQUAL(res.result(), http::status::ok);
        BOOST_CHECK(!res.body().empty());
        
        // Verify JSON structure based on actual API
        auto json_response = nlohmann::json::parse(res.body());
        BOOST_TEST_MESSAGE("Response: " << json_response.dump());
        
        stream.socket().shutdown(tcp::socket::shutdown_both);
        
    } catch (std::exception const &e) {
        BOOST_FAIL("End-to-end HTTP test failed: " << e.what());
    }
}

BOOST_AUTO_TEST_CASE(TestFullSystemWebSocketFlow) {
    if (!server_running) {
        BOOST_TEST_MESSAGE("Server not running, skipping end-to-end WebSocket test. "
                          "Start shield server manually to run this test.");
        return;
    }
    
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
            {"data", {{"player_id", "e2e_test_player"}}}
        };
        
        ws.write(net::buffer(test_message.dump()));
        
        beast::flat_buffer buffer;
        ws.read(buffer);
        
        std::string response = boost::beast::buffers_to_string(buffer.data());
        BOOST_CHECK(!response.empty());
        
        auto json_response = nlohmann::json::parse(response);
        BOOST_TEST_MESSAGE("WebSocket Response: " << json_response.dump());
        
        ws.close(websocket::close_code::normal);
        
    } catch (std::exception const &e) {
        BOOST_FAIL("End-to-end WebSocket test failed: " << e.what());
    }
}

BOOST_AUTO_TEST_SUITE_END()