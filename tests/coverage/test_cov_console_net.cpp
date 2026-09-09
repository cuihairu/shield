#define BOOST_TEST_MODULE CovConsoleNet
#include <atomic>
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "shield/net/console_server.hpp"
#include "shield/net/console_session.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

using shield::net::ConsoleServer;
using shield::net::ConsoleSession;

const char* SOCK_PATH = "/tmp/shield-cov-console.sock";

void remove_socket() {
    std::error_code ec;
    std::filesystem::remove(SOCK_PATH, ec);
}

// Runs an io_context on a background thread until destroyed.
struct IoRunner {
    boost::asio::io_context& io;
    std::thread thread;

    explicit IoRunner(boost::asio::io_context& i) : io(i) {
        thread = std::thread([this] { io.run(); });
    }
    ~IoRunner() {
        io.stop();
        if (thread.joinable()) thread.join();
    }
};

// Unix-socket test client with line-oriented helpers.
struct ConsoleClient {
    boost::asio::io_context io;
    boost::asio::local::stream_protocol::socket socket{io};
    boost::asio::streambuf read_buf;

    bool connect(const std::string& path) {
        boost::system::error_code ec;
        socket.connect(boost::asio::local::stream_protocol::endpoint(path), ec);
        return !ec;
    }

    void send_raw(const std::string& data) {
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(data), ec);
    }

    void send_line(const std::string& line) { send_raw(line + "\n"); }

    // Reads one '\n'-terminated line; "" on error/timeout.
    std::string recv_line(int timeout_ms = 2000) {
        boost::system::error_code ec;
        std::size_t bytes = 0;
        boost::asio::async_read_until(
            socket, read_buf, '\n',
            [&](const boost::system::error_code& e, std::size_t n) {
                ec = e;
                bytes = n;
            });
        io.run_for(std::chrono::milliseconds(timeout_ms));
        io.stop();
        io.restart();
        if (ec || bytes == 0) return "";
        auto data = read_buf.data();
        std::string line(boost::asio::buffers_begin(data),
                         boost::asio::buffers_begin(data) + bytes);
        read_buf.consume(bytes);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        return line;
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

}  // namespace

// Unix-domain console sockets are POSIX-only: the whole suite is skipped on
// Windows (boost.asio local sockets compile there but cannot operate).
#ifndef _WIN32
BOOST_AUTO_TEST_SUITE(ConsoleNetCoverage)

BOOST_AUTO_TEST_CASE(LineHandlingStripsCarriageReturnAndSplits) {
    remove_socket();
    boost::asio::io_context io;

    std::atomic<int> lines{0};
    std::string first, second, third;

    ConsoleServer server(io, SOCK_PATH);
    server.set_on_line([&](std::shared_ptr<ConsoleSession>, std::string line) {
        ++lines;
        if (lines == 1) first = line;
        if (lines == 2) second = line;
        if (lines == 3) third = line;
    });
    server.start();
    IoRunner runner(io);
    BOOST_CHECK_EQUAL(server.session_count(), 0u);

    ConsoleClient client;
    BOOST_REQUIRE(client.connect(SOCK_PATH));
    BOOST_CHECK(wait_until([&] { return server.session_count() == 1; }));

    // CRLF line ending: the '\r' must be stripped.
    client.send_line("with-cr\r");
    // Two lines in a single write.
    client.send_raw("second\nthird\n");
    BOOST_CHECK(wait_until([&] { return lines.load() == 3; }));
    BOOST_CHECK_EQUAL(first, "with-cr");
    BOOST_CHECK_EQUAL(second, "second");
    BOOST_CHECK_EQUAL(third, "third");

    client.close();
    server.stop();
}

BOOST_AUTO_TEST_CASE(EchoRoundTripAndQueuedWrites) {
    remove_socket();
    boost::asio::io_context io;

    ConsoleServer server(io, SOCK_PATH);
    server.set_on_line(
        [](std::shared_ptr<ConsoleSession> session, std::string line) {
            // Several queued sends in one callback exercise the write queue
            // path.
            session->send_line("echo:" + line);
            session->send_line("again:" + line);
        });
    server.start();
    IoRunner runner(io);

    ConsoleClient client;
    BOOST_REQUIRE(client.connect(SOCK_PATH));
    BOOST_CHECK(wait_until([&] { return server.session_count() == 1; }));

    client.send_line("hi");
    BOOST_CHECK_EQUAL(client.recv_line(), "echo:hi");
    BOOST_CHECK_EQUAL(client.recv_line(), "again:hi");

    client.close();
    server.stop();
}

BOOST_AUTO_TEST_CASE(ClientDisconnectCleansSessionUp) {
    remove_socket();
    boost::asio::io_context io;

    ConsoleServer server(io, SOCK_PATH);
    server.set_on_line([](std::shared_ptr<ConsoleSession>, std::string) {});
    server.start();
    IoRunner runner(io);

    ConsoleClient client;
    BOOST_REQUIRE(client.connect(SOCK_PATH));
    BOOST_CHECK(wait_until([&] { return server.session_count() == 1; }));

    client.close();
    // EOF on the read side -> handle_close -> on_close -> session erased.
    BOOST_CHECK(wait_until([&] { return server.session_count() == 0; }));

    server.stop();
}

BOOST_AUTO_TEST_CASE(CloseAllSessionsDropsConnections) {
    remove_socket();
    boost::asio::io_context io;

    ConsoleServer server(io, SOCK_PATH);
    server.set_on_line([](std::shared_ptr<ConsoleSession> session,
                          std::string) { session->send_line("ok"); });
    server.start();
    IoRunner runner(io);

    ConsoleClient c1, c2;
    BOOST_REQUIRE(c1.connect(SOCK_PATH));
    BOOST_REQUIRE(c2.connect(SOCK_PATH));
    BOOST_CHECK(wait_until([&] { return server.session_count() == 2; }));

    // Direct close_all_sessions: both sessions are closed (posted to their
    // strands) and removed from the server.
    server.close_all_sessions();
    BOOST_CHECK(wait_until([&] { return server.session_count() == 0; }));

    // Both clients observe EOF.
    boost::asio::streambuf buf1, buf2;
    boost::system::error_code ec1, ec2;
    boost::asio::async_read(
        c1.socket, buf1,
        [&](const boost::system::error_code& e, size_t) { ec1 = e; });
    c1.io.run_for(1s);
    c1.io.stop();
    boost::asio::async_read(
        c2.socket, buf2,
        [&](const boost::system::error_code& e, size_t) { ec2 = e; });
    c2.io.run_for(1s);
    c2.io.stop();
    BOOST_CHECK(ec1 == boost::asio::error::eof);
    BOOST_CHECK(ec2 == boost::asio::error::eof);

    c1.close();
    c2.close();
    server.stop();
}

BOOST_AUTO_TEST_CASE(WriteAfterPeerCloseErrorsOut) {
    remove_socket();
    boost::asio::io_context io;

    std::shared_ptr<ConsoleSession> captured;

    ConsoleServer server(io, SOCK_PATH);
    server.set_on_line([&](std::shared_ptr<ConsoleSession> session,
                           std::string) { captured = session; });
    server.start();
    IoRunner runner(io);

    ConsoleClient client;
    BOOST_REQUIRE(client.connect(SOCK_PATH));
    client.send_line("capture");
    BOOST_CHECK(wait_until([&] { return captured != nullptr; }));

    // Peer disappears; wait until the read side noticed the EOF and closed.
    client.close();
    BOOST_CHECK(wait_until([&] { return server.session_count() == 0; }));

    // Writing into the dead socket must fail and run handle_close without
    // crashing (session kept alive by our captured pointer). is_alive() is
    // already false from the read-side EOF, so wait on wall-clock time for
    // the failed write handler to run before the io loop is stopped.
    captured->send_line("too-late");
    std::this_thread::sleep_for(200ms);
    BOOST_CHECK(!captured->is_alive());

    server.stop();
}

BOOST_AUTO_TEST_CASE(StopRemovesSocketFileAndAbortsAccept) {
    remove_socket();
    boost::asio::io_context io;

    // A stale socket file is unlinked on start.
    FILE* f = std::fopen(SOCK_PATH, "w");
    if (f) std::fclose(f);
    BOOST_CHECK(std::filesystem::exists(SOCK_PATH));

    ConsoleServer server(io, SOCK_PATH);
    server.start();
    IoRunner runner(io);
    BOOST_CHECK(std::filesystem::exists(SOCK_PATH));

    ConsoleClient client;
    BOOST_REQUIRE(client.connect(SOCK_PATH));
    BOOST_CHECK(wait_until([&] { return server.session_count() == 1; }));

    // stop() closes the acceptor (the pending accept completes with an error
    // while listening_ is already false) and shuts every session down.
    server.stop();
    BOOST_CHECK(
        wait_until([&] { return !std::filesystem::exists(SOCK_PATH); }));

    // stop() twice is a no-op.
    server.stop();
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(AcceptErrorWhileListeningIsLogged) {
    remove_socket();
    boost::asio::io_context io;

    ConsoleServer server(io, SOCK_PATH);
    server.start();
    IoRunner runner(io);

    // Exhaust the fd table (two slots in reserve: the client socket plus
    // headroom) and connect: the connection lands in the backlog, but
    // accept() fails with EMFILE, which is logged because the server is
    // still listening.
    ConsoleClient client;
    {
        std::vector<int> fds;
        for (;;) {
            int fd = ::dup(0);
            if (fd < 0) break;
            fds.push_back(fd);
        }
        if (fds.size() >= 2) {
            ::close(fds.back());
            fds.pop_back();
            ::close(fds.back());
            fds.pop_back();
        }

        BOOST_REQUIRE(client.connect(SOCK_PATH));
        std::this_thread::sleep_for(300ms);
        client.close();

        for (int fd : fds) ::close(fd);
    }

    // The accept loop stopped on error; the listener stays marked listening
    // and no session was ever created.
    BOOST_CHECK_EQUAL(server.session_count(), 0u);
    server.stop();
}
#endif

BOOST_AUTO_TEST_CASE(SessionStateHelpers) {
    remove_socket();
    boost::asio::io_context io;

    std::shared_ptr<ConsoleSession> captured;

    ConsoleServer server(io, SOCK_PATH);
    server.set_on_line([&](std::shared_ptr<ConsoleSession> session,
                           std::string) { captured = session; });
    server.start();
    IoRunner runner(io);

    ConsoleClient client;
    BOOST_REQUIRE(client.connect(SOCK_PATH));
    client.send_line("go");
    BOOST_CHECK(wait_until([&] { return captured != nullptr; }));

    // Lua REPL state helpers on the session.
    BOOST_CHECK(captured->is_alive());
    BOOST_CHECK_EQUAL(captured->attached_service(), "");
    BOOST_CHECK(!captured->is_attached());
    captured->set_attached_service("scene-1");
    BOOST_CHECK(captured->is_attached());
    BOOST_CHECK_EQUAL(captured->attached_service(), "scene-1");

    captured->append_multiline("local x = 1");
    captured->append_multiline(" + 2");
    BOOST_CHECK_EQUAL(captured->multiline_buffer(), "local x = 1 + 2");
    captured->clear_multiline();
    BOOST_CHECK(captured->multiline_buffer().empty());
    BOOST_CHECK_GT(captured->id(), 0u);

    // Explicit close() from the server side.
    captured->close();
    BOOST_CHECK(wait_until([&] { return !captured->is_alive(); }));

    client.close();
    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
#endif  // !_WIN32
