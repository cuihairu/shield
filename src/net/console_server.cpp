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
    : socket_path_(socket_path),
      state_(std::make_shared<SharedState>(io_context)) {}

ConsoleServer::~ConsoleServer() { stop(); }

ConsoleServer::SharedState::SharedState(boost::asio::io_context& io_context)
#ifndef _WIN32
    : acceptor(io_context) {
#else
    : acceptor(io_context) {
#endif
}

void ConsoleServer::start() {
#ifndef _WIN32
    // Remove stale socket file
    ::unlink(socket_path_.c_str());

    boost::asio::local::stream_protocol::endpoint ep(socket_path_);
    state_->acceptor.open(ep.protocol());
    state_->acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    state_->acceptor.bind(ep);
    state_->acceptor.listen();

    // Restrict to same user
    ::chmod(socket_path_.c_str(), 0600);
#else
    // Windows: use TCP loopback on a random port
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::address_v4::loopback(),
                                      0);
    state_->acceptor.open(ep.protocol());
    state_->acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    state_->acceptor.bind(ep);
    state_->acceptor.listen();
    std::cerr << "[console] Windows: listening on TCP loopback port "
              << state_->acceptor.local_endpoint().port() << std::endl;
#endif

    state_->listening.store(true, std::memory_order_release);
    do_accept(state_);
}

void ConsoleServer::stop() {
    if (!state_->listening.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    boost::system::error_code ec;
    state_->acceptor.close(ec);

    close_all_sessions();

#ifndef _WIN32
    ::unlink(socket_path_.c_str());
#endif
}

void ConsoleServer::do_accept(std::shared_ptr<SharedState> state) {
    if (!state->listening.load(  // GCOVR_EXCL_BR_LINE (defensive: only a
                                 // stop-vs-accept race reads listening==false
                                 // here: start() sets listening before
                                 // calling do_accept, and stop() closes the
                                 // acceptor so the callback exits on
            std::memory_order_acquire))  // the cancelled accept below)
        return;  // GCOVR_EXCL_LINE (only a stop-vs-accept race reaches this
                 // return: start() sets listening before calling, stop()
                 // closes the acceptor so the callback exits on
                 // operation_aborted before re-arming)

#ifndef _WIN32
    state->acceptor.async_accept(
        [state](boost::system::error_code ec,
                boost::asio::local::stream_protocol::socket socket) {
            if (ec) {
                if (state->listening.load(std::memory_order_acquire)) {
                    std::cerr << "[console] accept error: " << ec.message()
                              << std::endl;
                }
                return;
            }

            auto id = state->next_session_id.fetch_add(1);

            // Build session callbacks: on_line from dispatcher, on_close for
            // cleanup. The on_close lambda captures only the shared state,
            // never `this`: a pending async completion can outlive the
            // ConsoleServer and must not dereference a freed server.
            ConsoleSessionCallbacks cbs;
            {
                std::lock_guard<std::mutex> lock(state->callback_mutex);
                cbs.on_line = state->on_line;
            }
            cbs.on_close = [state](std::shared_ptr<ConsoleSession> session) {
                std::lock_guard<std::mutex> lock(state->sessions_mutex);
                auto it = std::find(state->sessions.begin(),
                                    state->sessions.end(), session);
                if (it != state->sessions.end()) {  // GCOVR_EXCL_BR_LINE
                                                    // (defensive: a session is
                                                    // closed exactly once)
                    state->sessions.erase(it);
                }
            };

            auto session = std::make_shared<ConsoleSession>(
                id, std::move(socket), std::move(cbs));

            {
                std::lock_guard<std::mutex> lock(state->sessions_mutex);
                state->sessions.push_back(session);
            }

            session->start();
            do_accept(state);
        });
#else
    state->acceptor.async_accept([state](boost::system::error_code ec,
                                         boost::asio::ip::tcp::socket socket) {
        if (ec) {
            if (state->listening.load(std::memory_order_acquire)) {
                std::cerr << "[console] accept error: " << ec.message()
                          << std::endl;
            }
            return;
        }

        auto id = state->next_session_id.fetch_add(1);
        std::cerr << "[console] Windows session " << id << " accepted"
                  << std::endl;
        socket.close();
        do_accept(state);
    });
#endif
}

size_t ConsoleServer::session_count() const {
    std::lock_guard<std::mutex> lock(state_->sessions_mutex);
    return state_->sessions.size();
}

void ConsoleServer::close_all_sessions() {
    std::lock_guard<std::mutex> lock(state_->sessions_mutex);
    for (auto& session : state_->sessions) {
        session->close();
    }
    state_->sessions.clear();
}

}  // namespace shield::net
