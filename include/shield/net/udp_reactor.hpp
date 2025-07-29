#pragma once

#include <boost/asio.hpp>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include "shield/protocol/udp_protocol_handler.hpp"

namespace shield::net {

class UdpReactor {
public:
    using UdpHandlerCreator = std::function<std::unique_ptr<shield::protocol::UdpProtocolHandler>(boost::asio::io_context&, uint16_t)>;

    UdpReactor(uint16_t port, size_t num_worker_threads = 1);
    ~UdpReactor();

    void start();
    void stop();

    void set_handler_creator(UdpHandlerCreator creator) { m_handler_creator = std::move(creator); }
    
    // Get the protocol handler for direct interaction
    shield::protocol::UdpProtocolHandler* get_handler() const { return m_handler.get(); }
    
    // Statistics
    bool is_running() const { return m_running; }
    uint16_t port() const { return m_port; }
    size_t worker_threads() const { return m_worker_threads.size(); }

private:
    boost::asio::io_context m_io_context;
    std::unique_ptr<shield::protocol::UdpProtocolHandler> m_handler;
    std::vector<std::thread> m_worker_threads;
    
    UdpHandlerCreator m_handler_creator;
    uint16_t m_port;
    size_t m_num_worker_threads;
    std::atomic<bool> m_running{false};
};

} // namespace shield::net