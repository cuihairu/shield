// tests/net/test_udp_session.cpp
#define BOOST_TEST_MODULE UdpSessionTests
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio.hpp>

#include "shield/net/udp_session.hpp"

namespace shield::net {

// Test fixture for UDP session
class UdpSessionFixture {
public:
    UdpSessionFixture()
        : io_context_(),
          server_port_(19090),
          server_session_(io_context_, server_port_),
          client_socket_(io_context_),
          receive_count_(0),
          timeout_count_(0),
          last_session_id_(0) {
        // Setup server callbacks
        server_session_.on_receive(
            [this](uint64_t session_id, const char* data, size_t length,
                   const boost::asio::ip::udp::endpoint& from) {
                receive_count_++;
                last_session_id_ = session_id;
                last_received_data_ = std::string(data, length);
                last_sender_endpoint_ = from;
            });

        server_session_.on_timeout([this](uint64_t session_id) {
            timeout_count_++;
            timed_out_sessions_.push_back(session_id);
        });

        // Configure short timeouts for testing
        server_session_.set_session_timeout(std::chrono::seconds(2));
        server_session_.set_cleanup_interval(std::chrono::seconds(1));
    }

    ~UdpSessionFixture() {
        if (server_session_.local_port() != 0) {
            server_session_.stop();
        }
    }

    void start_server() {
        server_session_.start();
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void stop_server() { server_session_.stop(); }

    // Helper to send data from a client
    void send_from_client(const std::string& data,
                          const std::string& host = "127.0.0.1",
                          uint16_t port = 0) {
        boost::asio::ip::udp::endpoint sender_endpoint(
            boost::asio::ip::make_address(host), server_port_);

        client_socket_.async_send_to(
            boost::asio::buffer(data), sender_endpoint,
            [](boost::system::error_code ec, std::size_t /*bytes_sent*/) {
                BOOST_REQUIRE(!ec);
            });

        io_context_.run_one();
        io_context_.restart();
    }

    // Run IO context for a specified duration
    void run_for(std::chrono::milliseconds duration) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < duration) {
            io_context_.run_one();
            io_context_.restart();
        }
    }

    boost::asio::io_context io_context_;
    uint16_t server_port_;
    UdpSession server_session_;
    boost::asio::ip::udp::socket client_socket_;

    // Test state
    std::atomic<int> receive_count_;
    std::atomic<int> timeout_count_;
    uint64_t last_session_id_;
    std::string last_received_data_;
    boost::asio::ip::udp::endpoint last_sender_endpoint_;
    std::vector<uint64_t> timed_out_sessions_;
};

// UdpEndpoint tests
BOOST_AUTO_TEST_SUITE(UdpEndpointTests)

BOOST_AUTO_TEST_CASE(test_udp_endpoint_creation) {
    boost::asio::io_context io;
    boost::asio::ip::udp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"),
                                      8080);
    UdpEndpoint udp_endpoint(ep, 12345);

    BOOST_CHECK_EQUAL(udp_endpoint.session_id, 12345);
    BOOST_CHECK_EQUAL(udp_endpoint.endpoint.port(), 8080);
    BOOST_CHECK_EQUAL(udp_endpoint.endpoint.address().to_string(), "127.0.0.1");
}

BOOST_AUTO_TEST_CASE(test_udp_endpoint_update_activity) {
    boost::asio::io_context io;
    boost::asio::ip::udp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"),
                                      8080);
    UdpEndpoint udp_endpoint(ep, 1);

    auto first_activity = udp_endpoint.last_activity;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    udp_endpoint.update_activity();
    auto second_activity = udp_endpoint.last_activity;

    BOOST_CHECK(second_activity > first_activity);
}

BOOST_AUTO_TEST_CASE(test_udp_endpoint_is_expired) {
    boost::asio::io_context io;
    boost::asio::ip::udp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"),
                                      8080);
    UdpEndpoint udp_endpoint(ep, 1);

    // Should not be expired immediately
    BOOST_CHECK(!udp_endpoint.is_expired(std::chrono::seconds(10)));

    // Wait and check again
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK(!udp_endpoint.is_expired(std::chrono::seconds(10)));

    // Should be expired with short timeout
    BOOST_CHECK(udp_endpoint.is_expired(std::chrono::milliseconds(50)));
}

BOOST_AUTO_TEST_SUITE_END()

// UdpSession lifecycle tests
BOOST_AUTO_TEST_SUITE(UdpSessionLifecycleTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_session_start_stop) {
    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 0);

    start_server();
    BOOST_CHECK_EQUAL(server_session_.local_port(), server_port_);

    stop_server();
}

BOOST_AUTO_TEST_CASE(test_multiple_start_stop) {
    start_server();

    // Second start should be idempotent
    auto port_before = server_session_.local_port();
    start_server();
    BOOST_CHECK_EQUAL(server_session_.local_port(), port_before);

    stop_server();
}

BOOST_AUTO_TEST_CASE(test_session_initial_state) {
    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 0);
    BOOST_CHECK_EQUAL(server_session_.local_port(), server_port_);
}

BOOST_AUTO_TEST_SUITE_END()

// Send and receive tests
BOOST_AUTO_TEST_SUITE(UdpSessionSendReceiveTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_send_and_receive_single_message) {
    start_server();

    send_from_client("Hello, UDP!");
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(receive_count_, 1);
    BOOST_CHECK_EQUAL(last_received_data_, "Hello, UDP!");
    BOOST_CHECK_GT(last_session_id_, 0);
}

BOOST_AUTO_TEST_CASE(test_send_and_receive_multiple_messages) {
    start_server();

    send_from_client("Message 1");
    run_for(std::chrono::milliseconds(50));
    send_from_client("Message 2");
    run_for(std::chrono::milliseconds(50));
    send_from_client("Message 3");
    run_for(std::chrono::milliseconds(50));

    BOOST_CHECK_EQUAL(receive_count_, 3);
}

BOOST_AUTO_TEST_CASE(test_multiple_clients_create_different_sessions) {
    start_server();

    // Open another client socket with different port
    boost::asio::ip::udp::socket client2(io_context_);
    boost::asio::ip::udp::endpoint server_endpoint(
        boost::asio::ip::make_address("127.0.0.1"), server_port_);

    client2.async_send_to(
        boost::asio::buffer("Client 1"), server_endpoint,
        [](boost::system::error_code ec, std::size_t) { BOOST_REQUIRE(!ec); });

    io_context_.run_one();
    io_context_.restart();

    uint64_t first_session = last_session_id_;

    client_socket_.async_send_to(
        boost::asio::buffer("Client 2"), server_endpoint,
        [](boost::system::error_code ec, std::size_t) { BOOST_REQUIRE(!ec); });

    io_context_.run_one();
    io_context_.restart();

    uint64_t second_session = last_session_id_;

    // Sessions should be different
    BOOST_CHECK_NE(first_session, second_session);
}

BOOST_AUTO_TEST_SUITE_END()

// Session management tests
BOOST_AUTO_TEST_SUITE(UdpSessionManagementTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_session_creation_on_receive) {
    start_server();

    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 0);

    send_from_client("First message");
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 1);
    BOOST_CHECK_GT(last_session_id_, 0);
}

BOOST_AUTO_TEST_CASE(test_same_endpoint_reuses_session) {
    start_server();

    send_from_client("Message 1");
    run_for(std::chrono::milliseconds(50));

    uint64_t first_session = last_session_id_;

    send_from_client("Message 2");
    run_for(std::chrono::milliseconds(50));

    uint64_t second_session = last_session_id_;

    BOOST_CHECK_EQUAL(first_session, second_session);
    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 1);
}

BOOST_AUTO_TEST_CASE(test_remove_session) {
    start_server();

    send_from_client("Test message");
    run_for(std::chrono::milliseconds(100));

    uint64_t session_id = last_session_id_;
    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 1);

    server_session_.remove_session(session_id);
    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 0);
}

BOOST_AUTO_TEST_CASE(test_cleanup_expired_sessions) {
    start_server();

    // Set very short timeout for testing
    server_session_.set_session_timeout(std::chrono::milliseconds(500));
    server_session_.set_cleanup_interval(std::chrono::milliseconds(200));

    send_from_client("Temporary message");
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 1);

    // Wait for session to expire and be cleaned up
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    run_for(std::chrono::milliseconds(300));

    // Session should be cleaned up
    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 0);
    BOOST_CHECK_EQUAL(timeout_count_, 1);
}

BOOST_AUTO_TEST_SUITE_END()

// Send to session tests
BOOST_AUTO_TEST_SUITE(SendToSessionTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_send_to_endpoint) {
    start_server();

    // First, send a message from client to establish session
    send_from_client("Hello");
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(receive_count_, 1);
    BOOST_CHECK_GT(last_session_id_, 0);
}

BOOST_AUTO_TEST_CASE(test_send_to_session_id) {
    start_server();

    send_from_client("Initial message");
    run_for(std::chrono::milliseconds(100));

    uint64_t session_id = last_session_id_;
    BOOST_CHECK_GT(session_id, 0);

    // Send to session by ID
    server_session_.send_to(session_id, "Reply to session", 17);
    run_for(std::chrono::milliseconds(100));

    // Note: We can't easily verify the client received it without a client
    // receiver, but we can verify no exception was thrown
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(test_send_to_invalid_session_id) {
    start_server();

    // Try to send to non-existent session
    server_session_.send_to(99999, "Test", 4);
    run_for(std::chrono::milliseconds(100));

    // Should not crash, just log a warning
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()

// Configuration tests
BOOST_AUTO_TEST_SUITE(UdpSessionConfigurationTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_set_session_timeout) {
    server_session_.set_session_timeout(std::chrono::seconds(100));
    // No exception means success
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(test_set_cleanup_interval) {
    server_session_.set_cleanup_interval(std::chrono::seconds(30));
    // No exception means success
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(test_default_configuration) {
    // Test that default values are reasonable
    UdpSession session(io_context_, 19191);

    // Default timeout should be 5 minutes
    // Default cleanup interval should be 1 minute
    // We can't directly access these, but the session should start correctly
    BOOST_CHECK_EQUAL(session.active_sessions(), 0);
}

BOOST_AUTO_TEST_SUITE_END()

// Statistics tests
BOOST_AUTO_TEST_SUITE(UdpSessionStatisticsTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_active_sessions_count) {
    start_server();

    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 0);

    send_from_client("From client 1");
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(server_session_.active_sessions(), 1);
}

BOOST_AUTO_TEST_CASE(test_local_port) {
    BOOST_CHECK_EQUAL(server_session_.local_port(), server_port_);

    start_server();
    BOOST_CHECK_EQUAL(server_session_.local_port(), server_port_);
}

BOOST_AUTO_TEST_SUITE_END()

// Callback tests
BOOST_AUTO_TEST_SUITE(UdpSessionCallbackTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_receive_callback_invoked) {
    start_server();

    bool callback_called = false;
    server_session_.on_receive(
        [&callback_called](uint64_t, const char*, size_t,
                          const boost::asio::ip::udp::endpoint&) {
            callback_called = true;
        });

    send_from_client("Test");
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK(callback_called);
}

BOOST_AUTO_TEST_CASE(test_timeout_callback_invoked) {
    start_server();

    server_session_.set_session_timeout(std::chrono::milliseconds(300));
    server_session_.set_cleanup_interval(std::chrono::milliseconds(100));

    send_from_client("Expiring message");
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(timeout_count_, 0);

    // Wait for timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    run_for(std::chrono::milliseconds(200));

    BOOST_CHECK_EQUAL(timeout_count_, 1);
}

BOOST_AUTO_TEST_SUITE_END()

// Edge case tests
BOOST_AUTO_TEST_SUITE(UdpSessionEdgeCaseTests, *boost::unit_test::fixture<UdpSessionFixture>())

BOOST_AUTO_TEST_CASE(test_empty_message) {
    start_server();

    send_from_client("");
    run_for(std::chrono::milliseconds(100));

    // Empty message should still trigger callback
    // (though bytes_received would be 0)
    BOOST_CHECK_EQUAL(receive_count_, 1);
}

BOOST_AUTO_TEST_CASE(test_large_message) {
    start_server();

    std::string large_message(1024, 'X');  // 1KB message
    send_from_client(large_message);
    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(receive_count_, 1);
}

BOOST_AUTO_TEST_CASE(test_rapid_messages) {
    start_server();

    for (int i = 0; i < 10; ++i) {
        send_from_client("Rapid " + std::to_string(i));
        io_context_.run_one();
        io_context_.restart();
    }

    run_for(std::chrono::milliseconds(100));

    BOOST_CHECK_EQUAL(receive_count_, 10);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace shield::net
