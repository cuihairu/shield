// Default-config boot acceptance: the shipped config/app.yaml must not only
// validate (the --check-config smoke tests anchor that) but actually boot
// into an observable state — the echo listener answers a real TCP round
// trip. This anchors the "fresh checkout is useful out of the box" product
// promise: ./build.sh run gives a newcomer a port to talk to.
//
// The shipped file is booted with two environment-hermetic patches (console
// socket path and HTTP ops port, both fixed values in the shipped config
// that would collide across parallel runs) plus absolute script paths; the
// game listener (0.0.0.0:7900) is exercised exactly as shipped.
#define BOOST_TEST_MODULE DefaultConfigBootAcceptance
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

#ifndef SHIELD_SOURCE_DIR
#define SHIELD_SOURCE_DIR "."
#endif

using boost::asio::ip::tcp;
using json = nlohmann::json;

namespace {

// Ports/values patched out of the shipped config for hermetic parallel runs.
uint16_t free_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acc(
        io, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address("127.0.0.1"), 0));
    return acc.local_endpoint().port();
}

// Copies config/app.yaml to a temp file with the ops endpoints made
// hermetic and script paths made absolute. Everything else — actors, the
// echo listener port, routes — stays byte-identical to the shipped file.
std::optional<std::filesystem::path> patched_default_config() {
    const std::filesystem::path source =
        std::filesystem::path(SHIELD_SOURCE_DIR) / "config" / "app.yaml";
    std::ifstream in(source);
    if (!in.good()) {
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    const std::string scripts_dir =
        (std::filesystem::path(SHIELD_SOURCE_DIR) / "scripts").generic_string();
    const auto run_token = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string console_socket =
        (std::filesystem::temp_directory_path() /
         ("shield-boot-smoke-" + run_token + ".sock"))
            .generic_string();

    // ../scripts/<x>.lua (relative to the config dir) -> absolute
    const std::string rel = "../scripts/";
    std::string::size_type pos = 0;
    while ((pos = content.find(rel, pos)) != std::string::npos) {
        content.replace(pos, rel.size(), scripts_dir + "/");
        pos += scripts_dir.size() + 1;
    }
    // Fixed ops values -> per-run values.
    const std::string console_key = "/tmp/shield-console.sock";
    pos = content.find(console_key);
    if (pos == std::string::npos) {
        return std::nullopt;  // shipped config changed shape; update me
    }
    content.replace(pos, console_key.size(), console_socket);
    const std::string http_port_line = "  port: 8080";
    pos = content.find(http_port_line);
    if (pos == std::string::npos) {
        return std::nullopt;  // shipped config changed shape; update me
    }
    content.replace(pos, http_port_line.size(),
                    "  port: " + std::to_string(free_port()));

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() /
        ("shield_default_boot-" + run_token + ".yaml");
    std::ofstream of(out, std::ios::trunc);
    of << content;
    of.close();
    return out;
}

// idlen frame helpers (same wire contract as test_client_rpc_e2e).
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

std::optional<std::pair<std::uint32_t, std::string>> read_frame(
    tcp::socket& socket, std::vector<std::uint8_t>& buffer,
    std::chrono::steady_clock::time_point deadline) {
    while (std::chrono::steady_clock::now() < deadline) {
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

BOOST_AUTO_TEST_CASE(DefaultConfigBootsObservableEchoListener) {
    const auto cfg_path = patched_default_config();
    BOOST_REQUIRE_MESSAGE(cfg_path.has_value(),
                          "could not materialize patched default config");

    shield::bootstrap::RuntimeConfig rc;
    rc.config_files = {cfg_path->string()};
    BOOST_REQUIRE_MESSAGE(shield::bootstrap::initialize(rc),
                          "bootstrap with the shipped default config failed");

    // The shipped echo listener answers on 7900 exactly as configured.
    // bootstrap::initialize returns once listeners are up, but retry the
    // connect briefly so a slow accept-loop startup cannot flake the suite.
    boost::asio::io_context io;
    boost::system::error_code ec;
    tcp::socket client(io);
    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        client = tcp::socket(io);
        client.connect(
            tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 7900),
            ec);
        connected = !ec;
        if (!connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    BOOST_REQUIRE_MESSAGE(
        connected, "connect to default echo listener failed: " << ec.message());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    boost::asio::write(client,
                       boost::asio::buffer(make_frame(
                           1, json{{"hello", "shield"}, {"n", 7}}.dump())),
                       ec);
    BOOST_REQUIRE(!ec);

    std::vector<std::uint8_t> inbox;
    const auto reply = read_frame(client, inbox, deadline);
    BOOST_REQUIRE_MESSAGE(reply.has_value(), "no echo_result frame received");
    BOOST_CHECK_EQUAL(reply->first, 100u);

    const json body = json::parse(reply->second);
    BOOST_CHECK_EQUAL(body.value<std::int64_t>("seq", 0), 1);
    BOOST_CHECK(body.value<std::int64_t>("time_ms", 0) > 0);
    const json data = body.value("data", json::object());
    BOOST_CHECK_EQUAL(data.value("hello", ""), "shield");
    BOOST_CHECK_EQUAL(data.value("n", 0), 7);
    // Pure business body: the route never travels inside it.
    BOOST_CHECK(!body.contains("route"));
    BOOST_CHECK(!body.contains("route_id"));

    client.close(ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    shield::bootstrap::shutdown();
    std::filesystem::remove(*cfg_path);
}
