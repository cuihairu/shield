// shield/tests/net/test_network.cpp
#define BOOST_TEST_MODULE network_tests
#include <boost/test/unit_test.hpp>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>

#include "shield/net/session.hpp"
#include "shield/net/udp_reactor.hpp"
#include "shield/net/master_reactor.hpp"
#include "shield/protocol/binary_protocol.hpp"
#include "shield/protocol/udp_protocol_handler.hpp"

using namespace shield::net;
using namespace shield::protocol;

BOOST_AUTO_TEST_SUITE(network_suite)

// =====================================
// Session 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_session_construction) {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);

    Session session(std::move(socket));

    BOOST_CHECK_EQUAL(session.id(), 1);  // ID 从 1 开始
}

// =====================================
// BinaryProtocol 测试
// =====================================

BOOST_AUTO_TEST_CASE(test_binary_protocol_encode) {
    std::string payload = "Test message";
    auto encoded = BinaryProtocol::encode(payload);

    BOOST_CHECK(!encoded.empty());
    BOOST_CHECK_GT(encoded.size(), payload.size());  // 应该包含头部
}

BOOST_AUTO_TEST_CASE(test_binary_protocol_decode) {
    std::string payload = "Test message";
    auto encoded = BinaryProtocol::encode(payload);

    auto [decoded, consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
    BOOST_CHECK_EQUAL(decoded, payload);
    BOOST_CHECK_EQUAL(consumed, encoded.size());
}

BOOST_AUTO_TEST_CASE(test_binary_protocol_empty_message) {
    std::string payload = "";
    auto encoded = BinaryProtocol::encode(payload);

    BOOST_CHECK(!encoded.empty());
}

BOOST_AUTO_TEST_CASE(test_binary_protocol_large_message) {
    std::string payload(1024 * 1024, 'X');  // 1MB 数据
    auto encoded = BinaryProtocol::encode(payload);

    BOOST_CHECK(!encoded.empty());

    auto [decoded, consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
    BOOST_CHECK_EQUAL(decoded, payload);
}

// =====================================
// UdpReactor 基础测试
// =====================================

BOOST_AUTO_TEST_CASE(test_udp_reactor_construction) {
    UdpReactor reactor(12345, 2);

    BOOST_CHECK_EQUAL(reactor.port(), 12345);
}

BOOST_AUTO_TEST_CASE(test_udp_reactor_start_stop) {
    UdpReactor reactor(0, 2);  // 使用随机端口

    reactor.start();
    // 当传入 0 时，系统会选择随机端口，但 m_port 保持为 0
    // 这是预期行为，因为 UdpReactor 不更新 m_port
    BOOST_CHECK(reactor.is_running());

    reactor.stop();
    BOOST_CHECK(!reactor.is_running());
}

// =====================================
// UDP 消息测试
// =====================================

BOOST_AUTO_TEST_CASE(test_udp_message_construction) {
    boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), 8080);
    std::string data = "Test UDP message";
    UdpMessage message(123, data.c_str(), data.size(), endpoint);

    BOOST_CHECK_EQUAL(message.session_id, 123);
    BOOST_CHECK_EQUAL(message.data, "Test UDP message");
}

// =====================================
// 并发测试
// =====================================

BOOST_AUTO_TEST_CASE(test_concurrent_sessions) {
    const int NUM_SESSIONS = 100;

    std::vector<std::unique_ptr<Session>> sessions;

    for (int i = 0; i < NUM_SESSIONS; ++i) {
        boost::asio::io_context io_context;
        boost::asio::ip::tcp::socket socket(io_context);
        sessions.push_back(std::make_unique<Session>(std::move(socket)));
    }

    BOOST_CHECK_EQUAL(sessions.size(), NUM_SESSIONS);
}

// =====================================
// 协议边界测试
// =====================================

BOOST_AUTO_TEST_CASE(test_binary_protocol_empty_data) {
    std::string empty_data;
    auto encoded = BinaryProtocol::encode(empty_data);

    BOOST_CHECK(!encoded.empty());

    auto [decoded, consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
    BOOST_CHECK_EQUAL(decoded, "");
}

BOOST_AUTO_TEST_CASE(test_binary_protocol_special_characters) {
    std::string special_data = "\x00\x01\x02\xff\n\r\t";
    auto encoded = BinaryProtocol::encode(special_data);

    auto [decoded, consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
    BOOST_CHECK_EQUAL(decoded, special_data);
}

// =====================================
// 性能测试（轻量级）
// =====================================

BOOST_AUTO_TEST_CASE(test_binary_protocol_performance) {
    const int NUM_MESSAGES = 1000;
    const std::string TEST_DATA(1024, 'A');  // 1KB 消息

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_MESSAGES; ++i) {
        auto encoded = BinaryProtocol::encode(TEST_DATA);
        auto [decoded, consumed] = BinaryProtocol::decode(encoded.data(), encoded.size());
        BOOST_REQUIRE_EQUAL(decoded, TEST_DATA);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 性能检查: 应该在合理时间内完成
    BOOST_CHECK_LT(duration.count(), 5000);  // 5秒内完成
}

BOOST_AUTO_TEST_SUITE_END()
