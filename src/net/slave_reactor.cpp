#include "shield/net/slave_reactor.hpp"

#include "shield/core/logger.hpp"

namespace shield::net {

SlaveReactor::SlaveReactor()
    : m_work_guard(boost::asio::make_work_guard(m_io_context)) {}

void SlaveReactor::run() {
    SHIELD_LOG_INFO << "SlaveReactor running on thread: "
                    << std::this_thread::get_id();
    m_io_context.run();
}

void SlaveReactor::stop() { m_io_context.stop(); }

void SlaveReactor::post_session(std::shared_ptr<Session> session) {
    boost::asio::post(m_io_context, [session]() { session->start(); });
}

}  // namespace shield::net
