// [SHIELD_NET] Listener implementation
#include "shield/net/listener.hpp"

#include "shield/log/logger.hpp"

#include <boost/asio/buffer.hpp>
#include <mutex>
#include <shared_mutex>

namespace shield::net {

std::atomic<SessionId> TcpListener::g_next_session_id{1};

TcpListener::TcpListener(boost::asio::io_context& io_context,
                         uint16_t port,
                         SessionCallbacks callbacks)
    : io_context_(io_context),
      acceptor_(io_context),
      port_(port),
      callbacks_(std::move(callbacks)),
      socket_(io_context) {

    boost::system::error_code ec;

    // Open acceptor
    acceptor_.open(boost::asio::ip::tcp::v4(), ec);
    if (ec) {
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_ERROR(log, "Failed to open acceptor: " + ec.message());
        return;
    }

    // Allow address reuse
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);

    // Bind to port
    acceptor_.bind(boost::asio::ip::tcp::endpoint(
        boost::asio::ip::tcp::v4(), port), ec);

    if (ec) {
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_ERROR(log, "Failed to bind to port " + std::to_string(port) +
                        ": " + ec.message());
        return;
    }

    // Start listening
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);

    if (ec) {
        auto& log = shield::log::get_logger("net");
        SHIELD_LOG_ERROR(log, "Failed to listen: " + ec.message());
        return;
    }
}

void TcpListener::start() {
    do_accept();
}

void TcpListener::stop() {
    boost::system::error_code ec;
    acceptor_.close(ec);

    std::unique_lock lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
        session->close("listener shutdown");
    }
    sessions_.clear();
}

void TcpListener::do_accept() {
    acceptor_.async_accept(socket_, [this](auto ec) {
        if (ec) {
            if (ec != boost::asio::error::operation_aborted) {
                auto& log = shield::log::get_logger("net");
                SHIELD_LOG_ERROR(log, "Accept error: " + ec.message());
            }
            return;
        }

        // Check connection limit.
        {
            std::shared_lock lock(sessions_mutex_);
            if (max_connections_ > 0 && sessions_.size() >= max_connections_) {
                last_rejection_ = "connection_limit";
                auto& log = shield::log::get_logger("net");
                SHIELD_LOG_WARNING(log, "Connection rejected: limit reached (" +
                                  std::to_string(max_connections_) + ")");
                boost::system::error_code close_ec;
                socket_.close(close_ec);
                do_accept();
                return;
            }
        }

        // Check per-IP limit.
        auto remote_ep = socket_.remote_endpoint();
        std::string remote_ip = remote_ep.address().to_string();
        {
            std::shared_lock lock(sessions_mutex_);
            if (max_per_ip_ > 0) {
                auto it = ip_counts_.find(remote_ip);
                if (it != ip_counts_.end() && it->second >= max_per_ip_) {
                    last_rejection_ = "ip_limit";
                    auto& log = shield::log::get_logger("net");
                    SHIELD_LOG_WARNING(log, "Connection rejected: IP limit for " +
                                      remote_ip + " (" + std::to_string(max_per_ip_) + ")");
                    boost::system::error_code close_ec;
                    socket_.close(close_ec);
                    do_accept();
                    return;
                }
            }
        }

        // Create session
        SessionId id = g_next_session_id.fetch_add(1);
        auto session = std::make_shared<TcpSession>(
            id, std::move(socket_), callbacks_);

        // Store session
        {
            std::unique_lock lock(sessions_mutex_);
            sessions_[id] = session;
            ++ip_counts_[remote_ip];
        }

        // Start session
        session->start();

        // Accept next
        do_accept();
    });
}

std::shared_ptr<Session> TcpListener::find_session(SessionId id) const {
    std::shared_lock lock(sessions_mutex_);
    auto it = sessions_.find(id);
    return it != sessions_.end() ? it->second : nullptr;
}

void TcpListener::broadcast(const std::vector<uint8_t>& data) {
    std::shared_lock lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
        if (session->is_alive()) {
            session->send(data);
        }
    }
}

bool TcpListener::kick_session(SessionId id, std::string reason) {
    std::unique_lock lock(sessions_mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        it->second->close(std::move(reason));
        sessions_.erase(it);
        return true;
    }
    return false;
}

void TcpListener::on_session_close(std::shared_ptr<Session> session,
                                   std::string reason) {
    std::unique_lock lock(sessions_mutex_);
    auto it = sessions_.find(session->id());
    if (it != sessions_.end()) {
        // Decrement IP count.
        std::string ip = session->remote_addr().ip;
        auto ip_it = ip_counts_.find(ip);
        if (ip_it != ip_counts_.end()) {
            if (ip_it->second > 1) {
                --ip_it->second;
            } else {
                ip_counts_.erase(ip_it);
            }
        }
        sessions_.erase(it);
    }
}

}  // namespace shield::net
