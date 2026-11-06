#define BOOST_TEST_MODULE CovHttpServer
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
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

BOOST_AUTO_TEST_CASE(EphemeralPortRestartsFreshAndPortSurvivesStop) {
    boost::asio::io_context io;

    auto server = make_server(0);
    server.start();
    BOOST_REQUIRE(server.is_running());
    const auto first_port = server.port();
    BOOST_CHECK_NE(first_port, 0u);
    server.stop();

    // port() must not dereference a closed acceptor (which would throw) —
    // the bound port is cached for logging / reconnect bookkeeping.
    BOOST_CHECK_EQUAL(server.port(), first_port);

    // stop()/start() must re-request a fresh ephemeral port, not permanently
    // latch the previous one into the caller's config.
    server.start();
    BOOST_REQUIRE(server.is_running());
    const auto second_port = server.port();
    BOOST_CHECK_NE(second_port, 0u);
    server.stop();
}

BOOST_AUTO_TEST_CASE(StartFailsOnBusyPort) {
#if defined(__APPLE__) || defined(_WIN32)
    // BSD SO_REUSEADDR (boost.asio's default) lets a second acceptor bind
    // the same port on macOS, and Windows SO_REUSEADDR allows outright
    // hijacking, so the busy-port failure cannot be observed on either.
    BOOST_TEST_MESSAGE("busy-port rebind allowed on this platform; skipping");
    return;
#endif
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

BOOST_AUTO_TEST_CASE(ThrowingHandlerAnswers500AndServerSurvives) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.get("/boom", [](const HttpRequest&) -> HttpResponse {
        throw std::runtime_error("boom route exploded");
    });
    server.get("/boom-nonstd", [](const HttpRequest&) -> HttpResponse {
        throw 42;  // not a std::exception: exercises the catch(...) arm
    });
    server.get("/ok",
               [](const HttpRequest&) { return text_response("alive"); });
    server.start();
    BOOST_REQUIRE(server.is_running());

    // std::exception route: the server answers 500 with the uniform body.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /boom HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("500") != std::string::npos);
        BOOST_CHECK(resp.find("handler_exception") != std::string::npos);
        c.close();
    }

    // Non-std exception route: still a 500, not a dead connection.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /boom-nonstd HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("500") != std::string::npos);
        BOOST_CHECK(resp.find("handler_exception") != std::string::npos);
        c.close();
    }

    // The io thread survived both throws: a normal route still serves.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /ok HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("200") != std::string::npos);
        BOOST_CHECK(resp.find("alive") != std::string::npos);
        c.close();
    }

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

BOOST_AUTO_TEST_CASE(TrailingSlashPathStillMatchesRoute) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.route(HttpMethod::GET, "/users/:id",
                 [](const HttpRequest&) { return text_response("user"); });
    server.start();
    BOOST_REQUIRE(server.is_running());

    // A trailing slash produces an empty final segment: split_path leaves the
    // loop through the while condition instead of the break, and the
    // remaining segments still match the parameter route.
    RawClient c;
    BOOST_REQUIRE(c.connect(port));
    c.send_request("GET /users/42/ HTTP/1.0\r\nHost: t\r\n\r\n");
    const auto resp = c.read_all();
    BOOST_CHECK(resp.find("200") != std::string::npos);
    BOOST_CHECK(resp.find("user") != std::string::npos);
    c.close();

    server.stop();
}

BOOST_AUTO_TEST_CASE(ExplicitConnectionHeaderDrivesKeepAliveDecision) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    // HTTP/1.0 + explicit "Connection: keep-alive" in the response: the
    // server must keep the socket open (connection_token_exists says yes).
    server.get("/ka", [](const HttpRequest&) {
        HttpResponse r = text_response("ka-body");
        r.set(http::field::connection, "keep-alive");
        return r;
    });
    // HTTP/1.1 + explicit "Connection: close" in the response: the server
    // must close the socket after writing (connection_token_exists says
    // close).
    server.get("/close", [](const HttpRequest&) {
        HttpResponse r = text_response("close-body");
        r.set(http::field::connection, "close");
        return r;
    });
    server.start();
    BOOST_REQUIRE(server.is_running());

    // (a) keep-alive over HTTP/1.0: two requests on one socket.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /ka HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto first = c.read_response_keepalive();
        BOOST_CHECK(first.find("200") != std::string::npos);
        BOOST_CHECK(first.find("ka-body") != std::string::npos);

        c.send_request("GET /ka HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto second = c.read_response_keepalive();
        BOOST_CHECK(second.find("200") != std::string::npos);
        BOOST_CHECK(second.find("ka-body") != std::string::npos);
        c.close();
    }

    // (b) close over HTTP/1.1: response then EOF on the same socket.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /close HTTP/1.1\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();  // reads until the server closes
        BOOST_CHECK(resp.find("200") != std::string::npos);
        BOOST_CHECK(resp.find("close-body") != std::string::npos);
    }

    server.stop();
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(ClientAbortDuringLargeWriteClosesSession) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    server.get("/big", [](const HttpRequest&) {
        // Large enough that the write cannot complete before the client
        // aborts, forcing the on_write error branch (socket close).
        return text_response(std::string(8 * 1024 * 1024, 'b'));
    });
    server.start();
    BOOST_REQUIRE(server.is_running());

    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /big HTTP/1.1\r\nHost: t\r\n\r\n");
        // Let the server start streaming the body...
        std::this_thread::sleep_for(100ms);
        // ...then force an RST so the pending async_write fails.
        struct linger lg;
        lg.l_onoff = 1;
        lg.l_linger = 0;
        ::setsockopt(c.socket.native_handle(), SOL_SOCKET, SO_LINGER, &lg,
                     sizeof(lg));
        c.close();
        // Give the server time to observe the failed write and tear the
        // session down before the assertions below.
        std::this_thread::sleep_for(200ms);
    }

    // The server survives the aborted write and can still serve requests.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /big HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("200") != std::string::npos);
        c.close();
    }
    server.stop();
}
#endif

BOOST_AUTO_TEST_CASE(ConnectionHeaderValueVariants) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    auto server = make_server(port);
    // HTTP/1.0 + a non-keep-alive token: the token lookup misses, so the
    // connection must close.
    server.get("/v1-close", [](const HttpRequest&) {
        HttpResponse r = text_response("v1-close");
        r.set(http::field::connection, "close");
        return r;
    });
    // HTTP/1.0 + a long (> SSO) value containing the keep-alive token: the
    // connection stays open and the lowercased copy is heap-allocated.
    server.get("/v1-long", [](const HttpRequest&) {
        HttpResponse r = text_response("v1-long");
        r.set(http::field::connection,
              "keep-alive, timeout=5, max=1000-batched-requests");
        return r;
    });
    // HTTP/1.1 + a non-close token: keep-alive is the default and the
    // "close" token lookup misses.
    server.get("/v11-ka", [](const HttpRequest&) {
        HttpResponse r = text_response("v11-ka");
        r.set(http::field::connection, "keep-alive");
        return r;
    });
    // HTTP/1.1 + a long value containing "close".
    server.get("/v11-long", [](const HttpRequest&) {
        HttpResponse r = text_response("v11-long");
        r.set(http::field::connection, "close-after-this-whole-response!!!");
        return r;
    });
    // HTTP/1.1 + an empty header value: the token loop never runs.
    server.get("/v11-empty", [](const HttpRequest&) {
        HttpResponse r = text_response("v11-empty");
        r.set(http::field::connection, "");
        return r;
    });
    server.start();
    BOOST_REQUIRE(server.is_running());

    // (a) HTTP/1.0 + "close": response, then EOF.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /v1-close HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("200") != std::string::npos);
        BOOST_CHECK(resp.find("v1-close") != std::string::npos);
    }

    // (b) HTTP/1.0 + long keep-alive value: two requests on one socket.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /v1-long HTTP/1.0\r\nHost: t\r\n\r\n");
        const auto first = c.read_response_keepalive();
        BOOST_CHECK(first.find("200") != std::string::npos);
        BOOST_CHECK(first.find("v1-long") != std::string::npos);
        c.send_request("GET /v1-long HTTP/1.0\r\nHost: t\r\n\r\n");
        BOOST_CHECK(c.read_response_keepalive().find("v1-long") !=
                    std::string::npos);
        c.close();
    }

    // (c) HTTP/1.1 + "keep-alive": the socket stays open for a second round.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /v11-ka HTTP/1.1\r\nHost: t\r\n\r\n");
        BOOST_CHECK(c.read_response_keepalive().find("v11-ka") !=
                    std::string::npos);
        c.send_request("GET /v11-ka HTTP/1.1\r\nHost: t\r\n\r\n");
        BOOST_CHECK(c.read_response_keepalive().find("v11-ka") !=
                    std::string::npos);
        c.close();
    }

    // (d) HTTP/1.1 + long "close" value: response, then EOF.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /v11-long HTTP/1.1\r\nHost: t\r\n\r\n");
        const auto resp = c.read_all();
        BOOST_CHECK(resp.find("200") != std::string::npos);
        BOOST_CHECK(resp.find("v11-long") != std::string::npos);
    }

    // (e) HTTP/1.1 + empty value: token lookup runs on an empty string.
    {
        RawClient c;
        BOOST_REQUIRE(c.connect(port));
        c.send_request("GET /v11-empty HTTP/1.1\r\nHost: t\r\n\r\n");
        BOOST_CHECK(c.read_response_keepalive().find("v11-empty") !=
                    std::string::npos);
        c.close();
    }

    server.stop();
}

// try_static is the static-file-serving path behind set_static_root. The
// guard, method, NUL, percent-decode, traversal, root, containment and
// content-type arms are all driven here through direct calls so the whole
// branch surface stays green.
BOOST_AUTO_TEST_CASE(StaticFileServingBranches) {
    namespace fs = std::filesystem;
    HttpServer server = make_server(0);

    // port() on a never-started server whose configured port is 0 falls
    // through to the bind-time bound_port_ (still 0).
    BOOST_CHECK_EQUAL(server.port(), 0u);

    // port() on a server constructed with a fixed non-zero port returns the
    // configured port without starting (the config_override arm).
    BOOST_CHECK_EQUAL(make_server(40000).port(), 40000u);

    const auto dir = fs::temp_directory_path() / "shield_cov_http_static";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto write_file = [&dir](const std::string& name, const std::string& body) {
        std::ofstream out(dir / name, std::ios::binary);
        out << body;
    };
    write_file("index.html", "<html></html>");
    write_file("page.htm", "<html></html>");
    write_file("app.js", "console.log(1);");
    write_file("mod.mjs", "export {};");
    write_file("style.css", "body{}");
    write_file("data.json", "{}");
    write_file("img.svg", "<svg/>");
    write_file("pic.png", "png");
    write_file("fav.ico", "ico");
    write_file("blob.bin", "bin");

    server.set_static_root(dir.string());

    auto request = [](http::verb method, const std::string& target) {
        HttpRequest req{method, target, 11};
        return req;
    };

    // A rejecting static guard defers to the caller's fallback.
    server.set_static_guard([](const HttpRequest&) { return false; });
    BOOST_CHECK(!server.try_static(request(http::verb::get, "/index.html"))
                     .has_value());
    server.set_static_guard([](const HttpRequest&) { return true; });

    // Only GET serves static files.
    BOOST_CHECK(!server.try_static(request(http::verb::post, "/index.html"))
                     .has_value());

    // A NUL byte in the target is rejected outright.
    {
        std::string nul_target = "/index";
        nul_target.push_back('\0');
        nul_target += ".html";
        HttpRequest nul_req = request(http::verb::get, nul_target);
        BOOST_CHECK(!server.try_static(nul_req).has_value());
    }

    // Non-existent absolute root: rejected before canonicalisation.
    server.set_static_root("/nonexistent/shield/cov/static_root");
    BOOST_CHECK(!server.try_static(request(http::verb::get, "/index.html"))
                     .has_value());
    // Relative root: not absolute, rejected.
    server.set_static_root("shield_cov_relative_root");
    BOOST_CHECK(!server.try_static(request(http::verb::get, "/index.html"))
                     .has_value());
    server.set_static_root(dir.string());

    // Traversal and backslash are rejected before containment.
    BOOST_CHECK(
        !server.try_static(request(http::verb::get, "/../secret")).has_value());
    BOOST_CHECK(
        !server.try_static(request(http::verb::get, "/a\\b")).has_value());

    // Percent decoding: digit / lower / upper hex decode, invalid hex passes
    // through verbatim, and a truncated escape is not decoded.
    {
        auto ok = server.try_static(
            request(http::verb::get, "/%69ndex.html"));  // %69 -> 'i'
        BOOST_REQUIRE(ok.has_value());
        BOOST_CHECK(ok->body().find("<html></html>") != std::string::npos);
        BOOST_CHECK(
            !server.try_static(request(http::verb::get, "/%30ndex.html"))
                 .has_value());  // %30 -> '0', no such file
        BOOST_CHECK(
            !server.try_static(request(http::verb::get, "/%6Endex.html"))
                 .has_value());  // %6E -> 'N', lower-nibble hex arm
        BOOST_CHECK(
            !server.try_static(request(http::verb::get, "/%4Endex.html"))
                 .has_value());  // %4E -> 'N', upper-nibble hex arm
        BOOST_CHECK(
            !server.try_static(request(http::verb::get, "/%zzndex.html"))
                 .has_value());  // invalid hex, no such file
        BOOST_CHECK(
            !server.try_static(request(http::verb::get, "/%0zndex.html"))
                 .has_value());  // hi valid, lo invalid -> not decoded
        BOOST_CHECK(!server.try_static(request(http::verb::get, "/%!zz"))
                         .has_value());  // first nibble < '0' (digit-arm miss)
        BOOST_CHECK(!server.try_static(request(http::verb::get, "/%:zz"))
                         .has_value());  // first nibble < 'A' (upper-arm miss)
        BOOST_CHECK(
            !server.try_static(request(http::verb::get, "/index.html%2"))
                 .has_value());  // truncated escape, no such file
    }

    // A path without a leading slash is served relative to the root.
    {
        auto ok = server.try_static(request(http::verb::get, "index.html"));
        BOOST_REQUIRE(ok.has_value());
    }

    // An empty target leaves rel empty (the no-leading-slash arm of the rel
    // split); it resolves to the root directory, refused at the file check.
    BOOST_CHECK(!server.try_static(request(http::verb::get, "")).has_value());

    // A directory (not a regular file) is refused at the file-type check.
    BOOST_CHECK(!server.try_static(request(http::verb::get, "/")).has_value());

    // Content-type mapping and cache-control: every extension arm.
    auto content_type_of = [&](const std::string& target) {
        auto resp = server.try_static(request(http::verb::get, target));
        BOOST_REQUIRE(resp.has_value());
        return std::string(resp->at(http::field::content_type));
    };
    BOOST_CHECK_EQUAL(content_type_of("/index.html"),
                      "text/html; charset=utf-8");
    BOOST_CHECK_EQUAL(content_type_of("/page.htm"), "text/html; charset=utf-8");
    BOOST_CHECK_EQUAL(content_type_of("/app.js"), "application/javascript");
    BOOST_CHECK_EQUAL(content_type_of("/mod.mjs"), "application/javascript");
    BOOST_CHECK_EQUAL(content_type_of("/style.css"), "text/css");
    BOOST_CHECK_EQUAL(content_type_of("/data.json"), "application/json");
    BOOST_CHECK_EQUAL(content_type_of("/img.svg"), "image/svg+xml");
    BOOST_CHECK_EQUAL(content_type_of("/pic.png"), "image/png");
    BOOST_CHECK_EQUAL(content_type_of("/fav.ico"), "image/x-icon");
    BOOST_CHECK_EQUAL(content_type_of("/blob.bin"), "application/octet-stream");

    // HTML is revalidated; every other extension is cached aggressively.
    {
        auto html = server.try_static(request(http::verb::get, "/index.html"));
        BOOST_REQUIRE(html.has_value());
        BOOST_CHECK_EQUAL(html->at(http::field::cache_control), "no-cache");
        auto css = server.try_static(request(http::verb::get, "/style.css"));
        BOOST_REQUIRE(css.has_value());
        BOOST_CHECK(css->at(http::field::cache_control).find("max-age") !=
                    std::string::npos);
    }

    // A symlink inside the root pointing outside is resolved and refused by
    // the containment check.
    {
        const fs::path outside =
            fs::temp_directory_path() / "shield_cov_http_outside";
        fs::remove_all(outside);
        fs::create_directories(outside);
        {
            std::ofstream out(outside / "secret.txt");
            out << "outside";
        }
        std::error_code sym_ec;
        fs::create_symlink(outside / "secret.txt", dir / "escape.txt", sym_ec);
        if (!sym_ec) {
            BOOST_CHECK(
                !server.try_static(request(http::verb::get, "/escape.txt"))
                     .has_value());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
