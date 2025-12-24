#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include "shield/protocol/protocol_handler.hpp"

namespace shield::http {

struct BeastHttpServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 8082;
    int threads = 0;  // 0 means hardware concurrency
    std::string root_path = "/";
    std::size_t max_request_size = 1024 * 1024;  // 1MB
};

class BeastHttpServer {
public:
    using RequestHandler =
        std::function<protocol::HttpResponse(const protocol::HttpRequest&)>;

    BeastHttpServer(BeastHttpServerConfig config, RequestHandler handler);
    ~BeastHttpServer();

    BeastHttpServer(const BeastHttpServer&) = delete;
    BeastHttpServer& operator=(const BeastHttpServer&) = delete;

    void start();
    void stop();
    bool is_running() const;

private:
    void do_accept();

    BeastHttpServerConfig m_config;
    RequestHandler m_handler;

    boost::asio::io_context m_ioc;
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_next_connection_id{1};
};

}  // namespace shield::http

