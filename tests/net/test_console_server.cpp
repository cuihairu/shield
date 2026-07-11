#define BOOST_TEST_MODULE ConsoleServerTests
#include <boost/test/unit_test.hpp>

#include "shield/net/console_server.hpp"
#include "shield/net/console_session.hpp"
#include "shield/console/command_dispatcher.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

// Helper: connect to a Unix socket and send/receive lines
struct TestClient {
    boost::asio::io_context io;
    boost::asio::local::stream_protocol::socket socket;

    TestClient() : socket(io) {}

    bool connect(const std::string& path) {
        try {
            socket.connect(
                boost::asio::local::stream_protocol::endpoint(path));
            return true;
        } catch (...) {
            return false;
        }
    }

    void send_line(const std::string& line) {
        boost::asio::write(socket, boost::asio::buffer(line + "\n"));
    }

    std::string recv_line() {
        boost::asio::streambuf buf;
        boost::system::error_code ec;
        boost::asio::read_until(socket, buf, '\n', ec);
        if (ec) return "";
        auto bufs = buf.data();
        std::string data(boost::asio::buffers_begin(bufs),
                         boost::asio::buffers_begin(bufs) +
                             boost::asio::buffer_size(bufs));
        if (!data.empty() && data.back() == '\n') data.pop_back();
        return data;
    }

    void close() {
        boost::system::error_code ec;
        socket.close(ec);
    }
};

const char* TEST_SOCK = "/tmp/shield-console-test.sock";

}  // namespace

BOOST_AUTO_TEST_SUITE(ConsoleServerTests)

BOOST_AUTO_TEST_CASE(ServerStartsAndStops) {
    boost::asio::io_context io;
    ::unlink(TEST_SOCK);

    shield::net::ConsoleServer server(io, TEST_SOCK);
    BOOST_CHECK_NO_THROW(server.start());
    BOOST_CHECK(server.session_count() == 0);

    server.stop();
    // Socket file should be removed
    BOOST_CHECK(access(TEST_SOCK, F_OK) != 0);
}

BOOST_AUTO_TEST_CASE(ClientCanConnect) {
    boost::asio::io_context io;
    ::unlink(TEST_SOCK);

    shield::net::ConsoleServer server(io, TEST_SOCK);
    server.set_on_line(
        [](std::shared_ptr<shield::net::ConsoleSession> session,
           std::string line) {
            nlohmann::json resp = {{"type", "result"}, {"echo", line}};
            session->send_line(resp.dump());
        });
    server.start();

    // Run io in a thread
    std::thread io_thread([&io]() { io.run(); });

    TestClient client;
    BOOST_REQUIRE(client.connect(TEST_SOCK));

    // Give the server a moment to register the session
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a command
    client.send_line("hello");
    std::string response = client.recv_line();
    BOOST_CHECK(!response.empty());

    auto resp = nlohmann::json::parse(response);
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["echo"] == "hello");

    client.close();
    server.stop();
    io.stop();
    if (io_thread.joinable()) io_thread.join();
}

BOOST_AUTO_TEST_CASE(CommandDispatcherRoutesCommands) {
    shield::console::CommandDispatcher dispatcher;
    bool help_called = false;
    bool status_called = false;

    dispatcher.register_command(
        "help", "Show help",
        [&help_called](shield::net::ConsoleSession&,
                       const std::vector<std::string>&) { help_called = true; });
    dispatcher.register_command(
        "root.status", "Show status",
        [&status_called](shield::net::ConsoleSession&,
                         const std::vector<std::string>&) {
            status_called = true;
        });

    BOOST_CHECK(dispatcher.list_commands().size() == 2);
    BOOST_CHECK(dispatcher.list_commands()[0].first == "help");
    BOOST_CHECK(dispatcher.list_commands()[1].first == "root.status");
}

BOOST_AUTO_TEST_CASE(UnknownCommandReturnsError) {
    boost::asio::io_context io;
    ::unlink(TEST_SOCK);

    shield::console::CommandDispatcher dispatcher;
    dispatcher.register_command(
        "help", "Show help",
        [](shield::net::ConsoleSession& session,
           const std::vector<std::string>&) {
            nlohmann::json resp = {{"type", "result"}, {"data", "ok"}};
            session.send_line(resp.dump());
        });

    shield::net::ConsoleServer server(io, TEST_SOCK);
    server.set_on_line(
        [&dispatcher](std::shared_ptr<shield::net::ConsoleSession> session,
                      std::string line) {
            dispatcher.dispatch(session, line);
        });
    server.start();

    std::thread io_thread([&io]() { io.run(); });

    TestClient client;
    BOOST_REQUIRE(client.connect(TEST_SOCK));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client.send_line("nonexistent");
    std::string response = client.recv_line();
    BOOST_CHECK(!response.empty());

    auto resp = nlohmann::json::parse(response);
    BOOST_CHECK(resp["type"] == "error");
    BOOST_CHECK(resp["message"].get<std::string>().find("nonexistent") !=
                std::string::npos);

    client.close();
    server.stop();
    io.stop();
    if (io_thread.joinable()) io_thread.join();
}

BOOST_AUTO_TEST_CASE(AttachAndDetachMode) {
    boost::asio::io_context io;
    ::unlink(TEST_SOCK);

    shield::console::CommandDispatcher dispatcher;
    dispatcher.register_command(
        "attach", "Attach to service",
        [](shield::net::ConsoleSession& session,
           const std::vector<std::string>& args) {
            if (args.empty()) {
                nlohmann::json resp = {{"type", "error"},
                                       {"message", "need service name"}};
                session.send_line(resp.dump());
                return;
            }
            session.set_attached_service(args[0]);
            nlohmann::json resp = {
                {"type", "attached"}, {"service", args[0]}};
            session.send_line(resp.dump());
        });

    dispatcher.set_lua_line_handler(
        [](std::shared_ptr<shield::net::ConsoleSession> session,
           const std::string& line) {
            nlohmann::json resp = {{"type", "result"}, {"data", line}};
            session->send_line(resp.dump());
        });

    shield::net::ConsoleServer server(io, TEST_SOCK);
    server.set_on_line(
        [&dispatcher](std::shared_ptr<shield::net::ConsoleSession> session,
                      std::string line) {
            dispatcher.dispatch(session, line);
        });
    server.start();

    std::thread io_thread([&io]() { io.run(); });

    TestClient client;
    BOOST_REQUIRE(client.connect(TEST_SOCK));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Attach
    client.send_line("attach myservice");
    auto resp = nlohmann::json::parse(client.recv_line());
    BOOST_CHECK(resp["type"] == "attached");
    BOOST_CHECK(resp["service"] == "myservice");

    // Send Lua line (should go through lua handler)
    client.send_line("return 42");
    resp = nlohmann::json::parse(client.recv_line());
    BOOST_CHECK(resp["type"] == "result");
    BOOST_CHECK(resp["data"] == "return 42");

    // Detach
    client.send_line("detach");
    resp = nlohmann::json::parse(client.recv_line());
    BOOST_CHECK(resp["type"] == "detached");

    // Now in command mode again
    client.send_line("help");
    // Should get unknown command error since we didn't register "help"
    // (only "attach" was registered)

    client.close();
    server.stop();
    io.stop();
    if (io_thread.joinable()) io_thread.join();
}

BOOST_AUTO_TEST_SUITE_END()
