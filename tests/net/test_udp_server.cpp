#include "shield/core/logger.hpp"
#include "shield/net/udp_reactor.hpp"
#include "shield/protocol/udp_protocol_handler.hpp"
#include <chrono>
#include <iostream>
#include <thread>

class SimpleUdpHandler : public shield::protocol::UdpProtocolHandler {
public:
  SimpleUdpHandler(boost::asio::io_context &io_context, uint16_t port)
      : UdpProtocolHandler(io_context, port) {

    // Set up message handling
    set_message_callback([this](const shield::protocol::UdpMessage &message) {
      handle_udp_message(message);
    });

    set_session_timeout_callback(
        [this](uint64_t session_id) { handle_session_timeout(session_id); });
  }

private:
  void handle_udp_message(const shield::protocol::UdpMessage &message) {
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

void test_udp_server() {
  try {
    // Initialize logging
    shield::core::LogConfig log_config;
    shield::core::Logger::init(log_config);

    std::cout << "=== Testing UDP Server ===\n";

    // Create UDP reactor
    shield::net::UdpReactor reactor(12345, 2);

    // Set custom handler creator
    reactor.set_handler_creator(
        [](boost::asio::io_context &io_context, uint16_t port) {
          return std::make_unique<SimpleUdpHandler>(io_context, port);
        });

    // Start the reactor
    reactor.start();

    std::cout << "UDP server started on port " << reactor.port() << "\n";
    std::cout << "Send UDP packets to localhost:12345 to test\n";
    std::cout << "Press Enter to stop...\n";

    // Wait for user input
    std::cin.get();

    // Stop the reactor
    reactor.stop();

    std::cout << "UDP server stopped\n";

  } catch (const std::exception &e) {
    std::cerr << "UDP server test failed: " << e.what() << std::endl;
    throw;
  }
}

int main() {
  try {
    test_udp_server();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Test failed with exception: " << e.what() << std::endl;
    return 1;
  }
}