#pragma once

#include "shield/net/udp_session.hpp"
#include "shield/protocol/protocol_handler.hpp"
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace shield::protocol {

// UDP-specific message structure
struct UdpMessage {
  uint64_t session_id;
  std::string data;
  boost::asio::ip::udp::endpoint sender_endpoint;
  std::chrono::steady_clock::time_point timestamp;

  UdpMessage(uint64_t id, const char *msg_data, size_t length,
             const boost::asio::ip::udp::endpoint &endpoint)
      : session_id(id), data(msg_data, length), sender_endpoint(endpoint),
        timestamp(std::chrono::steady_clock::now()) {}
};

class UdpProtocolHandler : public IProtocolHandler {
public:
  using UdpMessageCallback = std::function<void(const UdpMessage &message)>;
  using UdpSessionTimeoutCallback = std::function<void(uint64_t session_id)>;

  explicit UdpProtocolHandler(boost::asio::io_context &io_context,
                              uint16_t port);
  virtual ~UdpProtocolHandler();

  // IProtocolHandler interface
  void handle_data(uint64_t connection_id, const char *data,
                   size_t length) override;
  void handle_connection(uint64_t connection_id) override;
  void handle_disconnection(uint64_t connection_id) override;
  bool send_data(uint64_t connection_id, const std::string &data) override;
  ProtocolType get_protocol_type() const override { return ProtocolType::UDP; }

  // UDP-specific methods
  void start();
  void stop();

  // Send data to specific endpoint
  void send_to_endpoint(const boost::asio::ip::udp::endpoint &endpoint,
                        const std::string &data);

  // Set UDP-specific callbacks
  void set_message_callback(UdpMessageCallback callback) {
    m_message_callback = std::move(callback);
  }
  void set_session_timeout_callback(UdpSessionTimeoutCallback callback) {
    m_session_timeout_callback = std::move(callback);
  }

  // Configuration
  void set_session_timeout(std::chrono::seconds timeout);
  void set_cleanup_interval(std::chrono::seconds interval);

  // Statistics
  size_t active_sessions() const;
  uint16_t local_port() const;

private:
  void on_udp_receive(uint64_t session_id, const char *data, size_t length,
                      const boost::asio::ip::udp::endpoint &from);
  void on_udp_timeout(uint64_t session_id);

  std::unique_ptr<shield::net::UdpSession> m_udp_session;
  UdpMessageCallback m_message_callback;
  UdpSessionTimeoutCallback m_session_timeout_callback;

  // Track session states for the IProtocolHandler interface
  std::unordered_set<uint64_t> m_active_sessions;
};

} // namespace shield::protocol