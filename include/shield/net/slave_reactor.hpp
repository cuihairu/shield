#pragma once

#include <boost/asio.hpp>
#include <memory>
#include "session.hpp"

namespace shield::net {

class SlaveReactor {
public:
    SlaveReactor();
    void run();
    void stop();
    void post_session(std::shared_ptr<Session> session);

private:
    boost::asio::io_context m_io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> m_work_guard;
}; 

} // namespace shield::net
