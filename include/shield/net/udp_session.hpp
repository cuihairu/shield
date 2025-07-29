#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace shield::net {

// UDP endpoint wrapper for session identification
struct UdpEndpoint {
    boost::asio::ip::udp::endpoint endpoint;
    uint64_t session_id;
    std::chrono::steady_clock::time_point last_activity;

    UdpEndpoint(const boost::asio::ip::udp::endpoint &ep, uint64_t id)
        : endpoint(ep),
          session_id(id),
          last_activity(std::chrono::steady_clock::now()) {}

    void update_activity() { last_activity = std::chrono::steady_clock::now(); }

    bool is_expired(std::chrono::seconds timeout) const {
        auto now = std::chrono::steady_clock::now();
        return (now - last_activity) > timeout;
    }
};

class UdpSession {
public:
    using ReceiveCallback =
        std::function<void(uint64_t session_id, const char *data, size_t length,
                           const boost::asio::ip::udp::endpoint &from)>;
    using TimeoutCallback = std::function<void(uint64_t session_id)>;

    explicit UdpSession(boost::asio::io_context &io_context, uint16_t port);
    ~UdpSession();

    void start();
    void stop();

    // Send data to a specific endpoint
    void send_to(const boost::asio::ip::udp::endpoint &endpoint,
                 const char *data, size_t length);
    void send_to(uint64_t session_id, const char *data, size_t length);

    // Set callbacks
    void on_receive(ReceiveCallback callback) {
        m_receive_callback = std::move(callback);
    }
    void on_timeout(TimeoutCallback callback) {
        m_timeout_callback = std::move(callback);
    }

    // Session management
    uint64_t get_or_create_session_id(
        const boost::asio::ip::udp::endpoint &endpoint);
    void remove_session(uint64_t session_id);
    void cleanup_expired_sessions();

    // Configuration
    void set_session_timeout(std::chrono::seconds timeout) {
        m_session_timeout = timeout;
    }
    void set_cleanup_interval(std::chrono::seconds interval) {
        m_cleanup_interval = interval;
    }

    // Statistics
    size_t active_sessions() const { return m_endpoint_sessions.size(); }
    uint16_t local_port() const { return m_socket.local_endpoint().port(); }

private:
    void do_receive();
    void schedule_cleanup();

    boost::asio::io_context &m_io_context;
    boost::asio::ip::udp::socket m_socket;
    boost::asio::steady_timer m_cleanup_timer;

    enum { max_length = 65536 };  // UDP max packet size
    char m_receive_buffer[max_length];
    boost::asio::ip::udp::endpoint m_sender_endpoint;

    // Session management
    std::unordered_map<std::string, uint64_t> m_endpoint_to_session;
    std::unordered_map<uint64_t, std::shared_ptr<UdpEndpoint>>
        m_endpoint_sessions;

    // Callbacks
    ReceiveCallback m_receive_callback;
    TimeoutCallback m_timeout_callback;

    // Configuration
    std::chrono::seconds m_session_timeout{300};  // 5 minutes default
    std::chrono::seconds m_cleanup_interval{60};  // 1 minute default

    // ID generation
    static std::atomic<uint64_t> s_next_session_id;

    bool m_running{false};
};

}  // namespace shield::net