#define BOOST_TEST_MODULE CovHttpServer
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "shield/net/http_server.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

using shield::net::HttpHandler;
using shield::net::HttpMethod;
using shield::net::HttpRequest;
using shield::net::HttpResponse;
using shield::net::HttpServer;
using shield::net::HttpServerConfig;

namespace http = shield::net::http;

HttpResponse text_response(std::string body) {
    HttpResponse r{http::status::ok, 11};
    r.body() = std::move(body);
    return r;
}

std::uint16_t reserve_ephemeral_port(boost::asio::io_context& io) {
    tcp::acceptor probe(io, tcp::endpoint(tcp::v4(), 0));
    return probe.local_endpoint().port();
}

// Minimal raw HTTP client: writes a request, reads the full response until
// EOF (HTTP/1.0 / Connection: close semantics).
struct RawClient {
    boost::asio::io_context io;
    tcp::socket socket{io};

    bool connect(std::uint16_t port) {
        boost::system::error_code ec;
        socket.connect(
            tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);
        return !ec;
    }

    void send_request(const std::string& request) {
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(request), ec);
    }

    std::string read_all() {
        std::string out;
        boost::system::error_code ec;
        for (;;) {
            char buf[4096];
            std::size_t n = socket.read_some(boost::asio::buffer(buf), ec);
            if (ec) break;
            out.append(buf, n);
        }
        return out;
    }

    // HTTP/1.1 keep-alive: read status line + headers + content-length body.
    std::string read_response_keepalive() {
        std::string head;
        boost::system::error_code ec;
        char ch = 0;
        while (head.size() < 65536 &&
               head.find("\r\n\r\n") == std::string::npos) {
            std::size_t n = socket.read_some(boost::asio::buffer(&ch, 1), ec);
            if (ec || n == 0) return head;
            head.push_back(ch);
        }

        std::string lower = head;
        std::transform(
            lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::size_t content_length = 0;
        auto pos = lower.find("content-length:");
        if (pos != std::string::npos) {
            content_length = std::stoul(lower.substr(pos + 15));
        }

        std::string body(content_length, '\0');
        if (content_length > 0) {
            std::size_t got = boost::asio::read(
                socket, boost::asio::buffer(&body[0], content_length), ec);
            body.resize(got);
        }
        return head + body;
    }

    void close() {
        boost::system::error_code ec;
        socket.close(ec);
    }
};

HttpServer make_server(std::uint16_t port) {
    HttpServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    return HttpServer(cfg);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(HttpServerCoverage)

BOOST_AUTO_TEST_CASE(StartStopLifecycle) {
    boost::asio::io_context io;

    // stop() before start() is a no-op.
    {
        auto server = make_server(0);
        server.stop();
        BOOST_CHECK(!server.is_running());
    }

    // start() twice: second call is a no-op.
    {
        const auto port = reserve_ephemeral_port(io);
        auto server = make_server(port);
        server.start();
        BOOST_CHECK(server.is_running());
        server.start();
        BOOST_CHECK(server.is_running());
        server.stop();
        BOOST_CHECK(!server.is_running());
        // stop() twice: second is a no-op.
        server.stop();
        BOOST_CHECK(!server.is_running());
    }
}

BOOST_AUTO_TEST_CASE(StartFailsOnBusyPort) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    tcp::acceptor holder(io, tcp::endpoint(tcp::v4(), port));
    auto server = make_server(port);
    server.start();
    BOOST_CHECK(!server.is_running());
    // A later stop() must stay a no-op after the failed start.
    server.stop();
    BOOST_CHECK(!server.is_running());
}

BOOST_AUTO_TEST_CASE(RoutesMethodsAndNotFound) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.get("/hello",
               [](const HttpRequest&) { return text_response("hi-get"); });
    server.post("/echo", [](const HttpRequest& req) {
        HttpResponse r{http::status::ok, 11};
        r.set(http::field::content_type, "text/plain");
        r.body() = req.body();
        return r;
    });
    server.route(HttpMethod::PUT, "/put",
                 [](const HttpRequest&) { return text_response("hi-put"); });
    server.route(HttpMethod::DELETE_, "/del",
                 [](const HttpRequest&) { return text_response("hi-delete"); });
    server.route(HttpMethod::PATCH, "/patch",
                 [](const HttpRequest&) { return text_response("hi-patch"); });
    server.route(HttpMethod::ANY, "/any",
                 [](const HttpRequest&) { return text_response("hi-any"); });
    // Method-specific route missing but an ANY route exists: the ANY route
    // acts as the fallback for that path.
    server.route(HttpMethod::ANY, "/multi",
                 [](const HttpRequest&) { return text_response("any-route"); });
    server.start();
    BOOST_REQUIRE(server.is_running());

    // GET with query string (query must be stripped for route matching).
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /hello?x=1&y=2 HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("200") != std::string::npos);
        BOOST_CHECK(resp.find("hi-get") != std::string::npos);
        BOOST_CHECK(resp.find("application/json") != std::string::npos);
        c.close();
    }

    // POST with a body, echoed back with the handler's own content type.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request(
            "POST /echo HTTP/1.0\r\nHost: t\r\nContent-Length: 5\r\n\r\nhello");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("200") != std::string::npos);
        BOOST_CHECK(resp.find("hello") != std::string::npos);
        BOOST_CHECK(resp.find("text/plain") != std::string::npos);
        c.close();
    }

    // Every remaining method, one connection each.
    for (const auto& [method, path, body] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"PUT", "/put", "hi-put"},
             {"DELETE", "/del", "hi-delete"},
             {"PATCH", "/patch", "hi-patch"},
             {"OPTIONS", "/any", "hi-any"}}) {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request(method + " " + path + " HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK_MESSAGE(resp.find("200") != std::string::npos,
                            method + " -> " + resp.substr(0, 40));
        BOOST_CHECK_MESSAGE(resp.find(body) != std::string::npos, method);
        c.close();
    }

    // Unknown path falls through to the built-in 404 JSON response.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /missing HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("404") != std::string::npos);
        BOOST_CHECK(resp.find("not_found") != std::string::npos);
        c.close();
    }

    // GET /multi has no GET-specific route: the ANY route answers.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /multi HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("200") != std::string::npos);
        BOOST_CHECK(resp.find("any-route") != std::string::npos);
        c.close();
    }

    // Garbage request line: beast read error, connection dropped.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("NOT-HTTP GARBAGE\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.empty());
        c.close();
    }

    // Connect and close without sending anything: EOF read error.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.close();
    }

    server.stop();
}

BOOST_AUTO_TEST_CASE(PathVariableRoutes) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.route(HttpMethod::GET, "/users/:id",
                 [](const HttpRequest&) { return text_response("user"); });
    server.route(HttpMethod::GET, "/users/:id/orders/:oid",
                 [](const HttpRequest&) { return text_response("order"); });
    // Exact route wins over a parameter pattern for the same path.
    server.route(HttpMethod::GET, "/users/me",
                 [](const HttpRequest&) { return text_response("exact"); });
    server.start();
    BOOST_REQUIRE(server.is_running());

    const auto get = [port](const std::string& target) {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET " + target + " HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        c.close();
        return resp;
    };

    const auto check = [&get](const std::string& target,
                              const std::string& body) {
        const auto resp = get(target);
        BOOST_CHECK_MESSAGE(resp.find("200") != std::string::npos &&
                                resp.find(body) != std::string::npos,
                            target + " -> " + resp.substr(0, 40));
    };
    check("/users/42", "user");
    check("/users/42/orders/7", "order");
    check("/users/me", "exact");
    // Wrong segment count does not match a parameter pattern.
    BOOST_CHECK(get("/users/42/orders").find("404") != std::string::npos);
    // Non-matching literal segment does not match either.
    BOOST_CHECK(get("/items/42").find("404") != std::string::npos);

    server.stop();
}

BOOST_AUTO_TEST_CASE(KeepAliveConnectionReusesSocket) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.get("/hello", [](const HttpRequest&) {
        return text_response("hi-keepalive");
    });
    server.start();

    RawClient c;
    BOOST_REQUIRE(c.connect(port));
    c.send_request("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n");
    const auto resp = c.read_response_keepalive();
    BOOST_CHECK(resp.find("200") != std::string::npos);
    BOOST_CHECK(resp.find("hi-keepalive") != std::string::npos);

    // Give the server time to see the close on its follow-up read.
    std::this_thread::sleep_for(100ms);
    c.close();
    server.stop();
}

BOOST_AUTO_TEST_CASE(DefaultHandlerCatchesUnmatchedRoutes) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.set_default_handler([](const HttpRequest& req) {
        return text_response("default:" + std::string(req.target()));
    });
    server.start();

    RawClient c;
    BOOST_REQUIRE(c.connect(port));
    c.send_request("GET /whatever HTTP/1.0\r\nHost: t\r\n\r\n");
    const auto resp = c.read_all();
    BOOST_CHECK(resp.find("200") != std::string::npos);
    BOOST_CHECK(resp.find("default:/whatever") != std::string::npos);
    c.close();

    server.stop();
}

BOOST_AUTO_TEST_CASE(LargePostBodyIsEchoed) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.post("/big", [](const HttpRequest& req) {
        return text_response(std::to_string(req.body().size()));
    });
    server.start();

    const std::string body(1024 * 1024, 'a');
    RawClient c;
    BOOST_REQUIRE(c.connect(port));
    c.send_request("POST /big HTTP/1.0\r\nHost: t\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\n\r\n" + body);
    const auto resp = c.read_all();
    BOOST_CHECK(resp.find("200") != std::string::npos);
    BOOST_CHECK(resp.find("1048576") != std::string::npos);
    c.close();

    server.stop();
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(AcceptErrorIsLogged) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.get("/x", [](const HttpRequest&) { return text_response("x"); });
    server.start();
    BOOST_REQUIRE(server.is_running());

    // The client object must exist before the fd table is exhausted; only
    // connect() happens inside. Two slots stay free (client socket +
    // headroom) so the kernel can queue the connection while the server's
    // accept() fails with EMFILE.
    RawClient c;
    {
        std::vector<int> fds;
        for (;;) {
            int fd = ::dup(0);
            if (fd < 0) break;
            fds.push_back(fd);
        }
        for (int i = 0; i < 2 && !fds.empty(); ++i) {
            ::close(fds.back());
            fds.pop_back();
        }

        BOOST_REQUIRE(c.connect(port));
        std::this_thread::sleep_for(200ms);
        c.close();

        for (int fd : fds) ::close(fd);
    }
    server.stop();
}
#endif

BOOST_AUTO_TEST_SUITE_END()
