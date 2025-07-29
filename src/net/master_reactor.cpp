#include "shield/net/master_reactor.hpp"

#include "shield/core/logger.hpp"

namespace shield::net {

MasterReactor::MasterReactor(const std::string& host, uint16_t port,
                             size_t num_slave_reactors)
    : m_acceptor(m_io_context), m_next_slave_reactor(0) {
    boost::asio::ip::tcp::resolver resolver(m_io_context);
    boost::asio::ip::tcp::endpoint endpoint =
        *resolver.resolve(host, std::to_string(port)).begin();
    m_acceptor.open(endpoint.protocol());
    m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    m_acceptor.bind(endpoint);
    m_acceptor.listen();

    for (size_t i = 0; i < num_slave_reactors; ++i) {
        m_slave_reactors.emplace_back(std::make_unique<SlaveReactor>());
        m_slave_threads.emplace_back(
            [this, i]() { m_slave_reactors[i]->run(); });
    }
}

MasterReactor::~MasterReactor() { stop(); }

void MasterReactor::start() {
    SHIELD_LOG_INFO << "MasterReactor starting...";
    do_accept();
    m_master_thread = std::thread([this]() { m_io_context.run(); });
}

void MasterReactor::stop() {
    m_io_context.stop();
    if (m_master_thread.joinable()) {
        m_master_thread.join();
    }
    for (auto& sr : m_slave_reactors) {
        sr->stop();
    }
    for (auto& t : m_slave_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    SHIELD_LOG_INFO << "MasterReactor stopped.";
}

void MasterReactor::do_accept() {
    m_acceptor.async_accept([this](boost::system::error_code ec,
                                   boost::asio::ip::tcp::socket socket) {
        if (!ec) {
            if (m_session_creator) {
                auto session = m_session_creator(std::move(socket));
                m_slave_reactors[m_next_slave_reactor]->post_session(session);
                m_next_slave_reactor =
                    (m_next_slave_reactor + 1) % m_slave_reactors.size();
            } else {
                SHIELD_LOG_WARN
                    << "No session creator set, dropping connection.";
            }
        } else {
            SHIELD_LOG_ERROR << "Accept error: " << ec.message();
        }
        do_accept();
    });
}

}  // namespace shield::net
