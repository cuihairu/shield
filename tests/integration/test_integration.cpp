#define BOOST_TEST_MODULE IntegrationTest
#include "shield/core/logger.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct IntegrationTestFixture {
  IntegrationTestFixture() {
    // 初始化日志系统
    shield::core::LogConfig log_config;
    log_config.level = shield::core::Logger::level_from_string("info");
    shield::core::Logger::init(log_config);

    // 等待服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  ~IntegrationTestFixture() {
    // 清理资源
  }
};

BOOST_FIXTURE_TEST_SUITE(IntegrationTests, IntegrationTestFixture)

// HTTP API测试
BOOST_AUTO_TEST_CASE(TestHttpGetPlayerInfo) {
  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    // 连接到HTTP服务器
    auto const results = resolver.resolve("localhost", "8081");
    stream.connect(results);

    // 发送HTTP GET请求
    http::request<http::string_body> req{http::verb::get, "/api/player/info",
                                         11};
    req.set(http::field::host, "localhost");
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::content_type, "application/json");

    http::write(stream, req);

    // 接收响应
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    // 验证响应
    BOOST_CHECK_EQUAL(res.result(), http::status::ok);
    BOOST_CHECK(!res.body().empty());

    // 解析JSON响应
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

    // 连接到HTTP服务器
    auto const results = resolver.resolve("localhost", "8081");
    stream.connect(results);

    // 准备POST数据
    nlohmann::json post_data = {{"action", "attack"}, {"target", "enemy1"}};
    std::string body = post_data.dump();

    // 发送HTTP POST请求
    http::request<http::string_body> req{http::verb::post, "/api/game/action",
                                         11};
    req.set(http::field::host, "localhost");
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();

    http::write(stream, req);

    // 接收响应
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    // 验证响应
    BOOST_CHECK_EQUAL(res.result(), http::status::ok);
    BOOST_CHECK(!res.body().empty());

    // 解析JSON响应
    auto json_response = nlohmann::json::parse(res.body());
    BOOST_CHECK(json_response.contains("status"));
    BOOST_CHECK_EQUAL(json_response["status"], "accepted");

    stream.socket().shutdown(tcp::socket::shutdown_both);

  } catch (std::exception const &e) {
    BOOST_FAIL("HTTP POST test failed: " << e.what());
  }
}

// WebSocket测试
BOOST_AUTO_TEST_CASE(TestWebSocketConnection) {
  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    websocket::stream<tcp::socket> ws(ioc);

    // 连接到WebSocket服务器
    auto const results = resolver.resolve("localhost", "8082");
    auto ep = net::connect(ws.next_layer(), results);

    // 执行WebSocket握手
    std::string host = "localhost:" + std::to_string(ep.port());
    ws.handshake(host, "/");

    // 发送测试消息
    nlohmann::json test_message = {
        {"type", "get_info"}, {"data", {{"player_id", "test_player_123"}}}};

    ws.write(net::buffer(test_message.dump()));

    // 接收响应
    beast::flat_buffer buffer;
    ws.read(buffer);

    std::string response = boost::beast::buffers_to_string(buffer.data());

    // 验证响应
    BOOST_CHECK(!response.empty());

    // 解析JSON响应
    auto json_response = nlohmann::json::parse(response);
    BOOST_CHECK(json_response.contains("success"));

    ws.close(websocket::close_code::normal);

  } catch (std::exception const &e) {
    BOOST_FAIL("WebSocket test failed: " << e.what());
  }
}

// Lua Actor集成测试
BOOST_AUTO_TEST_CASE(TestLuaActorIntegration) {
  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    websocket::stream<tcp::socket> ws(ioc);

    auto const results = resolver.resolve("localhost", "8082");
    auto ep = net::connect(ws.next_layer(), results);

    std::string host = "localhost:" + std::to_string(ep.port());
    ws.handshake(host, "/");

    // 测试不同类型的Lua actor消息
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

      std::string response = boost::beast::buffers_to_string(buffer.data());
      auto json_response = nlohmann::json::parse(response);

      // 验证Lua actor正确响应
      BOOST_CHECK(json_response.contains("success"));
      BOOST_CHECK(json_response.contains("data"));
    }

    ws.close(websocket::close_code::normal);

  } catch (std::exception const &e) {
    BOOST_FAIL("Lua Actor integration test failed: " << e.what());
  }
}

// 并发连接测试
BOOST_AUTO_TEST_CASE(TestConcurrentConnections) {
  const int num_connections = 5; // 减少连接数以避免资源问题
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
            {"data", {{"player_id", "concurrent_test_" + std::to_string(i)}}}};

        ws.write(net::buffer(test_message.dump()));

        beast::flat_buffer buffer;
        ws.read(buffer);

        std::string response = boost::beast::buffers_to_string(buffer.data());
        auto json_response = nlohmann::json::parse(response);

        if (json_response.contains("success")) {
          successful_connections++;
        }

        ws.close(websocket::close_code::normal);

      } catch (std::exception const &e) {
        // 连接失败，不增加成功计数
        std::cerr << "Concurrent connection " << i << " failed: " << e.what()
                  << std::endl;
      }
    });
  }

  // 等待所有线程完成
  for (auto &t : threads) {
    t.join();
  }

  // 验证大部分连接成功（允许一些失败）
  BOOST_CHECK_GE(successful_connections.load(), num_connections * 0.6);
}

BOOST_AUTO_TEST_SUITE_END()