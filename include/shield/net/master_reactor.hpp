#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <functional>
#include <thread>
#include <vector>

#include "session.hpp"
#include "slave_reactor.hpp"

namespace shield::net {

class MasterReactor {
public:
    using SessionCreator =
        std::function<std::shared_ptr<Session>(boost::asio::ip::tcp::socket)>;

    MasterReactor(const std::string &host, uint16_t port,
                  size_t num_slave_reactors);
    ~MasterReactor();

    void start();
    void stop();

    void set_session_creator(SessionCreator creator) {
        m_session_creator = std::move(creator);
    }

private:
    void do_accept();

    boost::asio::io_context m_io_context;
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::vector<std::unique_ptr<SlaveReactor>> m_slave_reactors;
    std::vector<std::thread> m_slave_threads;
    std::thread m_master_thread;
    std::atomic<size_t> m_next_slave_reactor;
    SessionCreator m_session_creator;
};

}  // namespace shield::net
