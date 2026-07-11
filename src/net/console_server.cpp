// [SHIELD_NET] Console server implementation
#include "shield/net/console_server.hpp"

#include <sys/stat.h>

#include <boost/asio.hpp>
#include <iostream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace shield::net {

ConsoleServer::ConsoleServer(boost::asio::io_context& io_context,
                             const std::string& socket_path)
    : io_(io_context),
#ifndef _WIN32
      acceptor_(io_context),
#else
      acceptor_(io_context),
#endif
      socket_path_(socket_path) {}

ConsoleServer::~ConsoleServer() { stop(); }

void ConsoleServer::start() {
#ifndef _WIN32
    // Remove stale socket file
    ::unlink(socket_path_.c_str());

    boost::asio::local::stream_protocol::endpoint ep(socket_path_);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();

    // Restrict to same user
    ::chmod(socket_path_.c_str(), 0600);
#else
    // Windows: use TCP loopback on a random port
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::address_v4::loopback(),
                                      0);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    std::cerr << "[console] Windows: listening on TCP loopback port "
              << acceptor_.local_endpoint().port() << std::endl;
#endif

    listening_ = true;
    do_accept();
}

void ConsoleServer::stop() {
    if (!listening_) return;
    listening_ = false;

    boost::system::error_code ec;
    acceptor_.close(ec);

    close_all_sessions();

#ifndef _WIN32
    ::unlink(socket_path_.c_str());
#endif
}

void ConsoleServer::do_accept() {
    if (!listening_) return;

#ifndef _WIN32
    acceptor_.async_accept(
        [this](boost::system::error_code ec,
               boost::asio::local::stream_protocol::socket socket) {
            if (ec) {
                if (listening_) {
                    std::cerr << "[console] accept error: " << ec.message()
                              << std::endl;
                }
                return;
            }

            auto id = next_session_id_.fetch_add(1);

            // Build session callbacks: on_line from dispatcher, on_close for
            // cleanup
            ConsoleSessionCallbacks cbs;
            cbs.on_line = on_line_;
            cbs.on_close =
                [this](std::shared_ptr<ConsoleSession> session) {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    auto it =
                        std::find(sessions_.begin(), sessions_.end(), session);
                    if (it != sessions_.end()) {
                        sessions_.erase(it);
                    }
                };

            auto session = std::make_shared<ConsoleSession>(
                id, std::move(socket), std::move(cbs));

            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                sessions_.push_back(session);
            }

            session->start();
            do_accept();
        });
#else
    acceptor_.async_accept(
        [this](boost::system::error_code ec,
               boost::asio::ip::tcp::socket socket) {
            if (ec) {
                if (listening_) {
                    std::cerr << "[console] accept error: " << ec.message()
                              << std::endl;
                }
                return;
            }

            auto id = next_session_id_.fetch_add(1);
            std::cerr << "[console] Windows session " << id << " accepted"
                      << std::endl;
            socket.close();
            do_accept();
        });
#endif
}

size_t ConsoleServer::session_count() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

void ConsoleServer::close_all_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& session : sessions_) {
        session->close();
    }
    sessions_.clear();
}

}  // namespace shield::net
