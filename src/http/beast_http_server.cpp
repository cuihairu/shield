#include "shield/http/beast_http_server.hpp"

#include <chrono>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "shield/log/logger.hpp"

namespace shield::http {
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

static std::string normalize_root_path(std::string root_path) {
    if (root_path.empty()) return "/";
    if (root_path.front() != '/') root_path.insert(root_path.begin(), '/');
    if (root_path.size() > 1 && root_path.back() == '/') root_path.pop_back();
    return root_path;
}

static std::string extract_path(std::string_view target) {
    auto q = target.find('?');
    return std::string(target.substr(0, q));
}

static protocol::HttpRequest to_protocol_request(
    uint64_t connection_id,
    const http::request<http::string_body>& req,
    const std::string& root_path) {
    protocol::HttpRequest out;
    out.connection_id = connection_id;
    out.method = std::string(req.method_string());
    out.version = (req.version() == 11) ? "HTTP/1.1" : "HTTP/1.0";

    std::string path = extract_path(std::string_view(req.target().data(),
                                                     req.target().size()));
    if (root_path != "/" && path.rfind(root_path, 0) == 0) {
        path.erase(0, root_path.size());
        if (path.empty()) path = "/";
        if (path.front() != '/') path.insert(path.begin(), '/');
    }
    out.path = std::move(path);

    out.headers.reserve(static_cast<size_t>(std::distance(req.begin(), req.end())));
    for (const auto& field : req) {
        out.headers.emplace(std::string(field.name_string()),
                            std::string(field.value()));
    }
    out.body = req.body();
    return out;
}

static http::response<http::string_body> to_beast_response(
    const http::request<http::string_body>& req,
    const protocol::HttpResponse& response) {
    http::response<http::string_body> res;
    res.version(req.version());
    res.result(http::int_to_status(
        static_cast<unsigned>(response.status_code)));
    if (!response.status_text.empty()) {
        res.reason(response.status_text);
    }

    for (const auto& [k, v] : response.headers) {
        res.set(k, v);
    }

    res.set(http::field::server, "shield");
    res.body() = response.body;
    res.keep_alive(req.keep_alive());
    res.prepare_payload();
    return res;
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket&& socket, BeastHttpServerConfig config,
                BeastHttpServer::RequestHandler handler, uint64_t connection_id)
        : m_stream(std::move(socket)),
          m_config(std::move(config)),
          m_handler(std::move(handler)),
          m_connection_id(connection_id),
          m_root_path(normalize_root_path(m_config.root_path)) {}

    void run() { do_read(); }

private:
    void do_read() {
        m_req = {};
        m_parser.emplace();
        m_parser->body_limit(m_config.max_request_size);
        m_stream.expires_after(std::chrono::seconds(30));

        http::async_read(
            m_stream, m_buffer, *m_parser,
            beast::bind_front_handler(&HttpSession::on_read,
                                      shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t /*bytes*/) {
        if (ec == http::error::end_of_stream) {
            return do_close();
        }
        if (ec) {
            SHIELD_LOG_DEBUG << "HTTP read error: " << ec.message();
            return;
        }

        m_req = m_parser->release();

        if (m_root_path != "/") {
            std::string path = extract_path(std::string_view(m_req.target().data(),
                                                             m_req.target().size()));
            if (path.rfind(m_root_path, 0) != 0) {
                protocol::HttpResponse not_found;
                not_found.status_code = 404;
                not_found.status_text = "Not Found";
                not_found.body = R"({"error":"Not Found"})";
                return write_response(std::move(not_found));
            }
        }

        protocol::HttpResponse response;
        try {
            auto preq =
                to_protocol_request(m_connection_id, m_req, m_root_path);
            response = m_handler ? m_handler(preq) : protocol::HttpResponse{};
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "HTTP handler exception: " << e.what();
            response.status_code = 500;
            response.status_text = "Internal Server Error";
            response.body = R"({"error":"Internal Server Error"})";
        }

        write_response(std::move(response));
    }

    void write_response(protocol::HttpResponse&& response) {
        auto res = std::make_shared<http::response<http::string_body>>(
            to_beast_response(m_req, response));

        http::async_write(
            m_stream, *res,
            beast::bind_front_handler(&HttpSession::on_write,
                                      shared_from_this(), res->need_eof(), res));
    }

    void on_write(bool close,
                  std::shared_ptr<http::response<http::string_body>> /*res*/,
                  beast::error_code ec, std::size_t /*bytes*/) {
        if (ec) {
            SHIELD_LOG_DEBUG << "HTTP write error: " << ec.message();
            return;
        }
        if (close) {
            return do_close();
        }
        do_read();
    }

    void do_close() {
        beast::error_code ec;
        m_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }

    beast::tcp_stream m_stream;
    beast::flat_buffer m_buffer;
    BeastHttpServerConfig m_config;
    BeastHttpServer::RequestHandler m_handler;
    uint64_t m_connection_id;
    std::string m_root_path;

    std::optional<http::request_parser<http::string_body>> m_parser;
    http::request<http::string_body> m_req;
};

BeastHttpServer::BeastHttpServer(BeastHttpServerConfig config,
                                 RequestHandler handler)
    : m_config(std::move(config)),
      m_handler(std::move(handler)),
      m_ioc(),
      m_acceptor(m_ioc) {}

BeastHttpServer::~BeastHttpServer() { stop(); }

bool BeastHttpServer::is_running() const { return m_running.load(); }

void BeastHttpServer::start() {
    if (m_running.exchange(true)) return;

    m_ioc.restart();

    const auto threads = (m_config.threads > 0)
                             ? m_config.threads
                             : static_cast<int>(std::thread::hardware_concurrency());

    tcp::resolver resolver(m_ioc);
    auto results = resolver.resolve(m_config.host, std::to_string(m_config.port));
    tcp::endpoint endpoint = *results.begin();

    beast::error_code ec;
    if (m_acceptor.is_open()) {
        m_acceptor.close(ec);
        ec = {};
    }
    m_acceptor.open(endpoint.protocol(), ec);
    if (ec) throw std::runtime_error("HTTP acceptor open failed: " + ec.message());

    m_acceptor.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) throw std::runtime_error("HTTP reuse_address failed: " + ec.message());

    m_acceptor.bind(endpoint, ec);
    if (ec) throw std::runtime_error("HTTP bind failed: " + ec.message());

    m_acceptor.listen(net::socket_base::max_listen_connections, ec);
    if (ec) throw std::runtime_error("HTTP listen failed: " + ec.message());

    SHIELD_LOG_INFO << "Beast HTTP server listening on " << m_config.host << ":"
                    << m_config.port;

    do_accept();

    m_threads.reserve(static_cast<size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        m_threads.emplace_back([this]() { m_ioc.run(); });
    }
}

void BeastHttpServer::stop() {
    if (!m_running.exchange(false)) return;

    beast::error_code ec;
    m_acceptor.close(ec);
    m_ioc.stop();

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
}

void BeastHttpServer::do_accept() {
    m_acceptor.async_accept(
        net::make_strand(m_ioc),
        [this](beast::error_code ec, tcp::socket socket) {
            if (!m_running.load()) return;
            if (!ec) {
                auto connection_id = m_next_connection_id.fetch_add(1);
                std::make_shared<HttpSession>(std::move(socket), m_config,
                                              m_handler, connection_id)
                    ->run();
            } else {
                SHIELD_LOG_ERROR << "HTTP accept error: " << ec.message();
            }
            do_accept();
        });
}

}  // namespace shield::http
