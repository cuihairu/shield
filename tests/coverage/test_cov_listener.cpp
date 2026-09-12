#define BOOST_TEST_MODULE CovListener
#include <atomic>
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "shield/net/listener.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

using shield::net::Session;
using shield::net::SessionCallbacks;
using shield::net::SessionId;
using shield::net::TcpListener;

std::vector<std::uint8_t> bytes(std::string_view s) {
    return {s.begin(), s.end()};
}

// Bind an ephemeral port, read it back, then release. The listener under test
// rebinds the same port immediately (SO_REUSEADDR), which keeps tests off
// fixed port numbers.
std::uint16_t reserve_ephemeral_port(boost::asio::io_context& io) {
    tcp::acceptor probe(io, tcp::endpoint(tcp::v4(), 0));
    return probe.local_endpoint().port();
}

struct Client {
    boost::asio::io_context io;
    tcp::socket socket{io};

    bool connect(std::uint16_t port) {
        boost::system::error_code ec;
        socket.connect(
            tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);
        return !ec;
    }

    void send(const std::vector<std::uint8_t>& data) {
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(data), ec);
    }

    // Reads exactly n raw bytes; empty on error/timeout.
    std::vector<std::uint8_t> read_exact(std::size_t n) {
        std::vector<std::uint8_t> out(n);
        boost::system::error_code ec;
        boost::asio::async_read(
            socket, boost::asio::buffer(out),
            [&](const boost::system::error_code& e, size_t) { ec = e; });
        io.run_for(2s);
        io.stop();
        io.restart();
        if (ec) return {};
        return out;
    }

    void close() {
        boost::system::error_code ec;
        socket.close(ec);
    }
};

bool wait_until(const std::function<bool()>& pred, int timeout_ms = 2000) {
    for (int i = 0; i < timeout_ms / 5; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

// RAII helper that exhausts the process file-descriptor table so accept()
// fails with EMFILE. One descriptor stays in reserve for the client socket;
// accept() then fails because no descriptor is left for the accepted socket.
class FdExhaustion {
public:
    FdExhaustion() {
#ifndef _WIN32
        for (;;) {
            int fd = ::dup(0);
            if (fd < 0) break;
            fds_.push_back(fd);
        }
        if (!fds_.empty()) {
            ::close(fds_.back());
            fds_.pop_back();
        }
#endif
    }

    ~FdExhaustion() {
#ifndef _WIN32
        for (int fd : fds_) ::close(fd);
#endif
    }

private:
    std::vector<int> fds_;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(ListenerCoverage)

BOOST_AUTO_TEST_CASE(BindFailureAndStartNoOp) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    // Hold the port so the listener under test fails to bind.
    tcp::acceptor holder(io, tcp::endpoint(tcp::v4(), port));

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    BOOST_CHECK(!listener.is_open());
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);
    BOOST_CHECK(listener.find_session(1) == nullptr);

    // start() on a non-listening listener is a no-op.
    listener.start();
    BOOST_CHECK(!listener.is_open());

    // stop() on a non-listening listener is harmless.
    listener.stop();
}

BOOST_AUTO_TEST_CASE(AcceptSessionRejectsRawIngressAndDisconnects) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<int> connects{0};
    std::atomic<int> disconnects{0};
    std::atomic<SessionId> last_id{0};

    SessionCallbacks callbacks;
    callbacks.on_connect = [&](std::shared_ptr<Session> s) {
        ++connects;
        last_id = s->id();
    };
    callbacks.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        ++disconnects;
    };

    TcpListener listener(io, port, callbacks);
    listener.start();
    BOOST_CHECK(listener.is_open());
    BOOST_CHECK_EQUAL(listener.port(), port);

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(100ms);
    BOOST_CHECK_EQUAL(connects.load(), 1);
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    auto session = listener.find_session(last_id.load());
    BOOST_REQUIRE(session != nullptr);
    BOOST_CHECK(session->is_alive());

    // Raw ingress without a protocol pipeline closes the session with a
    // stable error code (the old fallback silently dropped the bytes).
    c1.send(bytes("ping"));
    io.run_for(200ms);
    BOOST_CHECK(wait_until([&] { return disconnects.load() == 1; }));
    BOOST_CHECK(wait_until([&] { return listener.session_count() == 0; }));
    BOOST_CHECK(listener.find_session(last_id.load()) == nullptr);
    BOOST_CHECK_EQUAL(session->error_code(), "protocol_not_configured");

    listener.stop();
    io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(SharedIpCountDecrementsWithoutErase) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    listener.start();

    Client c1, c2;
    BOOST_REQUIRE(c1.connect(port));
    BOOST_REQUIRE(c2.connect(port));
    io.run_for(150ms);
    BOOST_CHECK_EQUAL(listener.session_count(), 2u);

    // First disconnect decrements the 127.0.0.1 count from 2 to 1 (no erase).
    c1.close();
    io.run_for(200ms);
    BOOST_CHECK(wait_until([&] { return listener.session_count() == 1; }));

    c2.close();
    io.run_for(200ms);
    BOOST_CHECK(wait_until([&] { return listener.session_count() == 0; }));

    listener.stop();
    io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(ConnectionLimitRejectsSecondClient) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    listener.set_max_connections(1);
    listener.start();

    Client c1, c2;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(100ms);
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    BOOST_REQUIRE(c2.connect(port));  // completed by the kernel backlog
    io.run_for(200ms);

    BOOST_CHECK(wait_until([&] {
        return listener.last_rejection_reason() == "connection_limit";
    }));
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);
    // The rejected socket was closed by the server: the read side sees EOF.
    boost::asio::streambuf buf;
    boost::system::error_code ec;
    boost::asio::async_read(
        c2.socket, buf,
        [&](const boost::system::error_code& e, size_t) { ec = e; });
    c2.io.run_for(1s);
    c2.io.stop();
    BOOST_CHECK(ec == boost::asio::error::eof);

    c1.close();
    c2.close();
    listener.stop();
    io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(PerIpLimitRejectsSecondClient) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    listener.set_max_per_ip(1);
    listener.start();

    Client c1, c2;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(100ms);
    BOOST_REQUIRE(c2.connect(port));
    io.run_for(200ms);

    BOOST_CHECK(wait_until(
        [&] { return listener.last_rejection_reason() == "ip_limit"; }));
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    c1.close();
    c2.close();
    listener.stop();
    io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(BroadcastReachesAllSessions) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    listener.start();

    Client c1, c2;
    BOOST_REQUIRE(c1.connect(port));
    BOOST_REQUIRE(c2.connect(port));
    io.run_for(150ms);
    BOOST_CHECK_EQUAL(listener.session_count(), 2u);

    listener.broadcast(bytes("notice"));
    io.run_for(200ms);

    auto b1 = c1.read_exact(6);
    auto b2 = c2.read_exact(6);
    BOOST_CHECK(std::string(b1.begin(), b1.end()) == "notice");
    BOOST_CHECK(std::string(b2.begin(), b2.end()) == "notice");

    // Broadcast is skipped for dead sessions without affecting live ones.
    c1.close();
    io.run_for(150ms);
    BOOST_CHECK(wait_until([&] { return listener.session_count() == 1; }));
    listener.broadcast(bytes("again"));
    io.run_for(200ms);
    auto b3 = c2.read_exact(5);
    BOOST_CHECK(std::string(b3.begin(), b3.end()) == "again");

    c2.close();
    listener.stop();
    io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(KickSessionClosesAndReports) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<SessionId> sid{0};
    std::string reason_seen;
    std::atomic<bool> disconnected{false};

    SessionCallbacks callbacks;
    callbacks.on_connect = [&](std::shared_ptr<Session> s) { sid = s->id(); };
    callbacks.on_disconnect = [&](std::shared_ptr<Session>,
                                  std::string_view reason) {
        reason_seen = std::string(reason);
        disconnected = true;
    };

    TcpListener listener(io, port, callbacks);
    listener.start();

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(100ms);

    BOOST_CHECK(!listener.kick_session(999999, "none"));
    BOOST_CHECK(listener.kick_session(sid.load(), "kicked"));
    io.run_for(200ms);

    BOOST_CHECK(disconnected.load());
    BOOST_CHECK_EQUAL(reason_seen, "kicked");
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);
    // The session was already removed from the map before close(); the
    // disconnect callback's removal finds nothing to do.
    BOOST_CHECK(!listener.kick_session(sid.load(), "again"));

    c1.close();
    listener.stop();
    io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(StopClosesSessionsAndAcceptor) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<bool> disconnected{false};
    std::string reason_seen;

    SessionCallbacks callbacks;
    callbacks.on_disconnect = [&](std::shared_ptr<Session>,
                                  std::string_view reason) {
        reason_seen = std::string(reason);
        disconnected = true;
    };

    TcpListener listener(io, port, callbacks);
    listener.start();

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(100ms);
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    listener.stop();
    // Drain the aborted accept and session-close handlers.
    io.run_for(200ms);

    BOOST_CHECK(disconnected.load());
    BOOST_CHECK_EQUAL(reason_seen, "listener shutdown");
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);

    // The session saw the server-side close as EOF (Windows may surface
    // it as connection_aborted instead, so any error counts).
    boost::asio::streambuf buf;
    boost::system::error_code ec;
    boost::asio::async_read(
        c1.socket, buf,
        [&](const boost::system::error_code& e, size_t) { ec = e; });
    c1.io.run_for(1s);
    c1.io.stop();
    BOOST_CHECK(ec != boost::system::errc::success);
    c1.close();
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(AcceptErrorWhileListeningIsLogged) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    listener.start();
    io.run_for(50ms);
    BOOST_REQUIRE(listener.is_open());

    // The client object (io_context + socket descriptors) must exist before
    // the fd table is exhausted; only connect() happens inside.
    Client c;
    {
        FdExhaustion exhaustion;
        BOOST_REQUIRE(c.connect(port));
        io.run_for(300ms);
        io.stop();
        c.close();
    }

    // The accept loop stopped on error; the listener stays marked open and
    // sessions were never created.
    BOOST_CHECK(listener.is_open());
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);
    listener.stop();
    io.restart();
    io.run_for(100ms);
}
#endif

BOOST_AUTO_TEST_SUITE_END()
