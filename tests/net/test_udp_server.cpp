#define BOOST_TEST_MODULE UdpServerTests
#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <string>

#include "shield/log/logger.hpp"
#include "shield/log/log_config.hpp"
#include "shield/net/udp_reactor.hpp"
#include "shield/protocol/udp_protocol_handler.hpp"

class SimpleUdpHandler : public shield::protocol::UdpProtocolHandler {
public:
    SimpleUdpHandler(boost::asio::io_context& io_context, uint16_t port)
        : UdpProtocolHandler(io_context, port) {
        // Set up message handling
        set_message_callback(
            [this](const shield::protocol::UdpMessage& message) {
                handle_udp_message(message);
            });

        set_session_timeout_callback([this](uint64_t session_id) {
            handle_session_timeout(session_id);
        });
    }

private:
    void handle_udp_message(const shield::protocol::UdpMessage& message) {
        SHIELD_LOG_INFO << "Received UDP message from session "
                        << message.session_id << ": " << message.data;

        // Echo the message back
        std::string response = "Echo: " + message.data;
        send_data(message.session_id, response);
    }

    void handle_session_timeout(uint64_t session_id) {
        SHIELD_LOG_INFO << "UDP session " << session_id << " timed out";
    }
};

BOOST_AUTO_TEST_CASE(test_udp_echo) {
    using boost::asio::ip::udp;
    using namespace std::chrono_literals;

    shield::log::LogConfig log_config;
    shield::log::Logger::init(log_config);

    shield::net::UdpReactor reactor(/*port=*/0, /*num_worker_threads=*/1);
    reactor.set_handler_creator(
        [](boost::asio::io_context& io_context, uint16_t port) {
            return std::make_unique<SimpleUdpHandler>(io_context, port);
        });
    reactor.start();

    auto* handler = reactor.get_handler();
    BOOST_REQUIRE(handler != nullptr);
    const uint16_t server_port = handler->local_port();
    BOOST_REQUIRE(server_port != 0);

    boost::asio::io_context ioc;
    udp::socket socket(ioc);
    socket.open(udp::v4());

    const udp::endpoint server_ep(boost::asio::ip::make_address("127.0.0.1"),
                                  server_port);

    std::string received;
    std::array<char, 2048> recv_buf{};
    udp::endpoint from;

    boost::asio::steady_timer timer(ioc);
    timer.expires_after(1500ms);
    timer.async_wait([&](const boost::system::error_code& ec) {
        if (!ec) {
            socket.cancel();
        }
    });

    socket.async_receive_from(
        boost::asio::buffer(recv_buf), from,
        [&](const boost::system::error_code& ec, std::size_t n) {
            if (!ec) {
                received.assign(recv_buf.data(), n);
            }
            timer.cancel();
        });

    const std::string payload = "hello";
    socket.send_to(boost::asio::buffer(payload), server_ep);

    ioc.run();

    reactor.stop();

    BOOST_CHECK_EQUAL(received, "Echo: " + payload);
}
