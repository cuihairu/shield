// Client RPC end-to-end acceptance: a real TCP client drives the
// hello_world closure over the wire — pre-auth login (route 1) on the auth
// entry listener, the coroutine-aware single-target bind to the player
// actor, and the room-served move (route 2 in, egress route 200 back).
//
// Wire contract asserted here: the route travels in the frame header
// (idlen envelope: 2-byte big-endian route id + 2-byte big-endian payload
// length) and the body is pure business JSON — no route field is ever
// written into it.
#define BOOST_TEST_MODULE ClientRpcE2E
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "shield/bootstrap/bootstrap.hpp"
#include "shield/shield.hpp"

#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif

using boost::asio::ip::tcp;
using json = nlohmann::json;

namespace {

uint16_t free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acc(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    return acc.local_endpoint().port();
}

std::string scripts_dir() {
    return (std::filesystem::path(SHIELD_SOURCE_DIR) / "examples" /
            "hello_world" / "scripts")
        .string();
}

// idlen frame: [route_id:2B BE][length:2B BE][body]
std::vector<std::uint8_t> make_frame(std::uint32_t route_id,
                                     const std::string& body) {
    std::vector<std::uint8_t> frame;
    frame.push_back(static_cast<std::uint8_t>((route_id >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(route_id & 0xFF));
    frame.push_back(static_cast<std::uint8_t>((body.size() >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(body.size() & 0xFF));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

bool wait_readable(tcp::socket& socket,
                   std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        if (socket.available(ec) > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Reads one idlen frame off the socket (accumulating across reads). Returns
// {route_id, body} or nullopt on deadline / disconnect.
std::optional<std::pair<std::uint32_t, std::string>> read_frame(
    tcp::socket& socket, std::vector<std::uint8_t>& buffer,
    std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
        // Parse whatever is already buffered.
        if (buffer.size() >= 4) {
            const std::uint32_t route_id =
                (static_cast<std::uint32_t>(buffer[0]) << 8) | buffer[1];
            const std::size_t length =
                (static_cast<std::size_t>(buffer[2]) << 8) | buffer[3];
            if (buffer.size() >= 4 + length) {
                std::string body(
                    buffer.begin() + 4,
                    buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
                buffer.erase(
                    buffer.begin(),
                    buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
                return std::make_pair(route_id, body);
            }
        }
        if (!wait_readable(socket, deadline)) {
            break;
        }
        boost::system::error_code ec;
        std::vector<std::uint8_t> chunk(4096);
        const auto n = socket.read_some(boost::asio::buffer(chunk), ec);
        if (ec) {
            break;
        }
        buffer.insert(buffer.end(), chunk.begin(),
                      chunk.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return std::nullopt;
}

}  // namespace

BOOST_AUTO_TEST_CASE(LoginBindMoveEgressLoop) {
    namespace fs = std::filesystem;

    const uint16_t port = free_port();
    const std::string scripts = scripts_dir();

    // The hello_world closure with an ephemeral listener port: auth is the
    // listener's auth entry, player is the post-bind target, room serves
    // business forwarded by player.
    const fs::path cfg_path =
        fs::temp_directory_path() / "shield_e2e_client_rpc.yaml";
    {
        std::ofstream out(cfg_path, std::ios::trunc);
        out << "app:\n"
               "  name: hello_world_e2e\n"
               "log:\n"
               "  level: \"warn\"\n"
               "  console: true\n"
               "actors:\n"
               "  - name: \"auth\"\n"
               "    script: \"" +
                   scripts +
                   "/auth.lua\"\n"
                   "    instances: 1\n"
                   "    network:\n"
                   "      tcp: \"127.0.0.1:" +
                   std::to_string(port) +
                   "\"\n"
                   "      protocol:\n"
                   "        name: hello.v1\n"
                   "        envelope:\n"
                   "          type: \"idlen\"\n"
                   "          route_id_bytes: 2\n"
                   "          length_bytes: 2\n"
                   "          endian: \"big\"\n"
                   "        body:\n"
                   "          codec: \"json\"\n"
                   "    rpc:\n"
                   "      routes:\n"
                   "        - id: 1\n"
                   "          name: \"login\"\n"
                   "          binding: \"login\"\n"
                   "          direction: \"c2s\"\n"
                   "          requires_auth: false\n"
                   "        - id: 100\n"
                   "          name: \"login_result\"\n"
                   "          binding: \"login_result\"\n"
                   "          direction: \"s2c\"\n"
                   "  - name: \"player\"\n"
                   "    script: \"" +
                   scripts +
                   "/player.lua\"\n"
                   "    instances: 1\n"
                   "    rpc:\n"
                   "      routes:\n"
                   "        - id: 2\n"
                   "          name: \"move\"\n"
                   "          binding: \"move\"\n"
                   "          direction: \"c2s\"\n"
                   "          requires_auth: true\n"
                   "  - name: \"room\"\n"
                   "    script: \"" +
                   scripts +
                   "/room.lua\"\n"
                   "    instances: 1\n"
                   "    rpc:\n"
                   "      routes:\n"
                   "        - id: 200\n"
                   "          name: \"move_result\"\n"
                   "          binding: \"move_result\"\n"
                   "          direction: \"s2c\"\n";
    }

    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg_path.string()};
    BOOST_REQUIRE_MESSAGE(shield::bootstrap::initialize(rc),
                          "bootstrap initialize failed");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);

    boost::asio::io_context io;
    tcp::socket client(io);
    boost::system::error_code ec;
    client.connect(
        tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port), ec);
    BOOST_REQUIRE_MESSAGE(!ec, "tcp connect failed: " << ec.message());

    // --- Pre-auth login (route 1): auth entry handles it and binds the
    // session to the player actor. ---
    boost::asio::write(client,
                       boost::asio::buffer(make_frame(
                           1, json{{"player_id", "e2e_player"}}.dump())),
                       ec);
    BOOST_REQUIRE(!ec);

    std::vector<std::uint8_t> inbox;
    const auto login_reply = read_frame(client, inbox, deadline);
    BOOST_REQUIRE_MESSAGE(login_reply.has_value(),
                          "no login_result frame received");
    BOOST_CHECK_EQUAL(login_reply->first, 100u);

    const json login_body = json::parse(login_reply->second);
    BOOST_CHECK_EQUAL(login_body.value("player_id", ""), "e2e_player");
    BOOST_CHECK(login_body.value("room_hint", "") == "hall");
    BOOST_CHECK(login_body.value("session_id", 0) > 0);
    // The route never travels in the body: pure business data only.
    BOOST_CHECK(!login_body.contains("route"));
    BOOST_CHECK(!login_body.contains("route_id"));
    BOOST_CHECK(!login_body.contains("route_name"));
    BOOST_CHECK(!login_body.contains("msg_id"));

    // --- Authenticated move (route 2): player forwards to room, room
    // answers through the s2c move_result helper (route 200). ---
    boost::asio::write(
        client,
        boost::asio::buffer(make_frame(2, json{{"x", 7}, {"y", 9}}.dump())),
        ec);
    BOOST_REQUIRE(!ec);

    const auto move_reply = read_frame(client, inbox, deadline);
    BOOST_REQUIRE_MESSAGE(move_reply.has_value(),
                          "no move_result frame received");
    BOOST_CHECK_EQUAL(move_reply->first, 200u);

    const json move_body = json::parse(move_reply->second);
    BOOST_CHECK_EQUAL(move_body.value("player_id", ""), "e2e_player");
    BOOST_CHECK_EQUAL(move_body.value("x", 0), 7);
    BOOST_CHECK_EQUAL(move_body.value("y", 0), 9);
    BOOST_CHECK_EQUAL(move_body.value("online", 0), 1);
    BOOST_CHECK(!move_body.contains("route"));
    BOOST_CHECK(!move_body.contains("route_id"));

    // Graceful teardown; the socket closing from our side must not trip the
    // shutdown.
    client.close(ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    shield::bootstrap::shutdown();
    std::filesystem::remove(cfg_path);
}
