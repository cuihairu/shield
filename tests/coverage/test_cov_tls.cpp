// Coverage-focused TLS tests: make_tls_server_context failure arms plus the
// loopback handshake matrix from docs/tls-design.md (success with a real
// frame roundtrip, plaintext peer rejected, handshake timeout, blocklist
// beats handshake). Self-signed fixtures live in tests/net/fixtures/tls.
#define BOOST_TEST_MODULE TlsCoverageTests
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "shield/net/listener.hpp"
#include "shield/net/tls_context.hpp"
#include "shield/transport/protocol.hpp"

namespace ssl = boost::asio::ssl;
using boost::asio::ip::tcp;
using shield::net::Session;
using shield::net::SessionCallbacks;
using shield::net::TcpListener;
using shield::transport::BodyCodecRegistry;
using shield::transport::DecodedBody;
using shield::transport::DispatchResult;
using shield::transport::EnvelopeKind;
using shield::transport::JsonBodyCodec;
using shield::transport::Packet;
using shield::transport::ProtocolPipeline;
using shield::transport::ProtocolProfile;
using shield::transport::RouteAction;
using shield::transport::RouteDirection;
using shield::transport::RouteEntry;
using shield::transport::RoutePolicy;
using shield::transport::RouteSource;
using shield::transport::RouteTable;

namespace {

const std::filesystem::path& fixture_dir() {
    static const std::filesystem::path dir =
        std::filesystem::path(SHIELD_SOURCE_DIR) / "tests/net/fixtures/tls";
    return dir;
}

std::uint16_t reserve_ephemeral_port(boost::asio::io_context& io) {
    tcp::acceptor probe(io, tcp::endpoint(tcp::v4(), 0));
    return probe.local_endpoint().port();
}

// LenPrefix envelope + JSON body codec, route 1001 "login" decoded locally.
// Mirrors the helper in test_cov_listener.cpp.
std::unique_ptr<ProtocolPipeline> make_json_pipeline() {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = true},
    });

    BodyCodecRegistry codecs;
    codecs.add(1, std::make_unique<JsonBodyCodec>());

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    profile.unknown_route_action = RouteAction::Drop;

    return std::make_unique<ProtocolPipeline>(
        std::move(profile), std::move(routes), std::move(codecs));
}

std::vector<std::uint8_t> encode_frame(ProtocolPipeline& pipe,
                                       std::string_view body) {
    Packet packet;
    packet.body.assign(body.begin(), body.end());
    auto encoded = pipe.encode(packet.ref());
    BOOST_REQUIRE(pipe.error().empty());
    return encoded;
}

// Blocking TLS client on its own io_context, so its synchronous operations
// never touch the listener's io thread.
struct TlsClient {
    boost::asio::io_context io;
    ssl::context ctx{make_client_context()};
    ssl::stream<tcp::socket> stream{io, ctx};

    bool connect(std::uint16_t port) {
        boost::system::error_code ec;
        stream.lowest_layer().connect(
            tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);
        if (ec) {
            return false;
        }
        stream.handshake(ssl::stream_base::client, ec);
        return !ec;
    }

    void send(const std::vector<std::uint8_t>& data) {
        boost::asio::write(stream, boost::asio::buffer(data));
    }

    // Blocking read of the next server push (echoes here are one small TLS
    // record); empty on EOF/error.
    std::vector<std::uint8_t> read_all() {
        std::uint8_t buf[512];
        boost::system::error_code ec;
        const std::size_t n = stream.read_some(boost::asio::buffer(buf), ec);
        if (ec) {
            return {};
        }
        return {buf, buf + n};
    }

    void close() {
        boost::system::error_code ec;
        stream.lowest_layer().close(ec);
    }

private:
    // The self-signed fixture is its own trust anchor: verifying against it
    // exercises the full server-certificate verification path. Configured
    // before the ssl::stream member binds the context.
    static ssl::context make_client_context() {
        ssl::context ctx(ssl::context::tls_client);
        ctx.load_verify_file((fixture_dir() / "localhost.crt").string());
        ctx.set_verify_mode(ssl::verify_peer);
        return ctx;
    }
};

// Spin the listener io until the predicate holds or the deadline lapses.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

std::shared_ptr<boost::asio::ssl::context> make_fixture_context() {
    std::shared_ptr<boost::asio::ssl::context> ctx;
    std::string error;
    BOOST_REQUIRE(shield::net::make_tls_server_context(
        (fixture_dir() / "localhost.crt").string(),
        (fixture_dir() / "localhost.key").string(), ctx, &error));
    return ctx;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(TlsCoverage)

// --- make_tls_server_context failure arms -----------------------------------

BOOST_AUTO_TEST_CASE(ContextRejectsEmptyPaths) {
    std::shared_ptr<boost::asio::ssl::context> ctx;
    std::string error = "unchanged";
    BOOST_CHECK(
        !shield::net::make_tls_server_context("", "k.pem", ctx, &error));
    BOOST_CHECK(error.find("must not be empty") != std::string::npos);
    BOOST_CHECK(
        !shield::net::make_tls_server_context("c.pem", "", ctx, &error));
    BOOST_CHECK(error.find("must not be empty") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ContextRejectsMissingCertificate) {
    std::shared_ptr<boost::asio::ssl::context> ctx;
    std::string error;
    BOOST_CHECK(!shield::net::make_tls_server_context(
        (fixture_dir() / "no-such-cert.pem").string(),
        (fixture_dir() / "localhost.key").string(), ctx, &error));
    BOOST_CHECK(error.find("failed to load certificate") != std::string::npos);
    BOOST_CHECK(ctx == nullptr);
}

BOOST_AUTO_TEST_CASE(ContextRejectsMissingKey) {
    std::shared_ptr<boost::asio::ssl::context> ctx;
    std::string error;
    BOOST_CHECK(!shield::net::make_tls_server_context(
        (fixture_dir() / "localhost.crt").string(),
        (fixture_dir() / "no-such-key.pem").string(), ctx, &error));
    BOOST_CHECK(error.find("failed to load private key") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ContextRejectsGarbagePem) {
    const auto dir =
        std::filesystem::temp_directory_path() / "shield_cov_tls_garbage";
    std::filesystem::create_directories(dir);
    const auto cert = dir / "garbage.crt";
    {
        std::ofstream out(cert);
        out << "this is not a pem file\n";
    }
    std::shared_ptr<boost::asio::ssl::context> ctx;
    std::string error;
    BOOST_CHECK(!shield::net::make_tls_server_context(
        cert.string(), (fixture_dir() / "localhost.key").string(), ctx,
        &error));
    BOOST_CHECK(error.find("failed to load certificate") != std::string::npos);
    std::filesystem::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(ContextRejectsKeyMismatch) {
    std::shared_ptr<boost::asio::ssl::context> ctx;
    std::string error;
    // Valid cert, valid-but-unrelated key: both loads succeed, the
    // cross-check must fail.
    BOOST_CHECK(!shield::net::make_tls_server_context(
        (fixture_dir() / "localhost.crt").string(),
        (fixture_dir() / "server2.key").string(), ctx, &error));
    BOOST_CHECK(error.find("mismatch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(ContextAcceptsFixturePair) {
    std::shared_ptr<boost::asio::ssl::context> ctx;
    std::string error;
    BOOST_CHECK(shield::net::make_tls_server_context(
        (fixture_dir() / "localhost.crt").string(),
        (fixture_dir() / "localhost.key").string(), ctx, &error));
    BOOST_CHECK(ctx != nullptr);

    // error is optional: a null out-param must work the same way.
    std::shared_ptr<boost::asio::ssl::context> ctx2;
    BOOST_CHECK(shield::net::make_tls_server_context(
        (fixture_dir() / "localhost.crt").string(),
        (fixture_dir() / "localhost.key").string(), ctx2, nullptr));
    BOOST_CHECK(ctx2 != nullptr);

    // The failure path tolerates a null error out-param too.
    std::shared_ptr<boost::asio::ssl::context> ctx3;
    BOOST_CHECK(!shield::net::make_tls_server_context(
        (fixture_dir() / "no-such-cert.pem").string(),
        (fixture_dir() / "localhost.key").string(), ctx3, nullptr));
}

// --- loopback handshake matrix ----------------------------------------------

BOOST_AUTO_TEST_CASE(TlsHandshakeServesFramesBothWays) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<int> connects{0};
    std::atomic<int> packets{0};
    std::atomic<bool> disconnected{false};

    SessionCallbacks callbacks;
    callbacks.create_protocol_pipeline = [] { return make_json_pipeline(); };
    callbacks.on_connect = [&](std::shared_ptr<Session>) { ++connects; };
    callbacks.on_packet = [&](std::shared_ptr<Session> session,
                              const DispatchResult&) {
        ++packets;
        // Echo raw bytes back so the TLS write path is exercised too.
        session->send({'o', 'k'});
    };
    callbacks.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    TcpListener listener(io, port, std::move(callbacks));
    listener.set_tls(make_fixture_context(), 5000);
    BOOST_CHECK(listener.tls_enabled());
    listener.start();

    std::thread io_thread([&io] { io.run(); });
    TlsClient client;
    BOOST_CHECK(client.connect(port));

    BOOST_CHECK(wait_until([&] { return connects.load() == 1; },
                           std::chrono::seconds(5)));
    BOOST_CHECK_EQUAL(listener.tls_handshakes_total(), 1u);
    BOOST_CHECK_EQUAL(listener.tls_handshake_failures_total(), 0u);
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    auto pipe = make_json_pipeline();
    client.send(encode_frame(*pipe, R"({"route":"login","payload":{}})"));
    BOOST_CHECK(wait_until([&] { return packets.load() == 1; },
                           std::chrono::seconds(5)));
    const auto echoed = client.read_all();
    BOOST_CHECK(!echoed.empty());

    client.close();
    BOOST_CHECK(wait_until([&] { return disconnected.load(); },
                           std::chrono::seconds(5)));
    listener.stop();
    io.stop();
    io_thread.join();
}

BOOST_AUTO_TEST_CASE(PlaintextClientIsRejectedWithoutSession) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<int> connects{0};

    SessionCallbacks callbacks;
    callbacks.create_protocol_pipeline = [] { return make_json_pipeline(); };
    callbacks.on_connect = [&](std::shared_ptr<Session>) { ++connects; };

    TcpListener listener(io, port, std::move(callbacks));
    listener.set_tls(make_fixture_context(), 5000);
    listener.start();

    std::thread io_thread([&io] { io.run(); });

    // A plain TCP peer that speaks first: the server handshake reads bytes
    // that are not a ClientHello and must close without creating a session.
    boost::asio::io_context client_io;
    tcp::socket client(client_io);
    client.connect(
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    boost::system::error_code write_ec;
    boost::asio::write(client, boost::asio::buffer("GET / HTTP/1.1\r\n\r\n"),
                       write_ec);
    BOOST_CHECK(!write_ec);

    BOOST_CHECK(
        wait_until([&] { return listener.tls_handshake_failures_total() == 1; },
                   std::chrono::seconds(5)));
    BOOST_CHECK_EQUAL(listener.tls_handshakes_total(), 0u);
    BOOST_CHECK_EQUAL(connects.load(), 0);
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);

    // The server closed: the client's next read fails (EOF or reset).
    std::uint8_t buf[64];
    boost::system::error_code read_ec;
    client.read_some(boost::asio::buffer(buf), read_ec);
    BOOST_CHECK(read_ec);

    listener.stop();
    io.stop();
    io_thread.join();
}

BOOST_AUTO_TEST_CASE(HandshakeTimeoutClosesSilentPeer) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<int> connects{0};
    SessionCallbacks callbacks;
    callbacks.create_protocol_pipeline = [] { return make_json_pipeline(); };
    callbacks.on_connect = [&](std::shared_ptr<Session>) { ++connects; };

    TcpListener listener(io, port, std::move(callbacks));
    listener.set_tls(make_fixture_context(), 200);  // short deadline on purpose
    listener.start();

    std::thread io_thread([&io] { io.run(); });

    // Connect (TCP done) and then send nothing: only the deadline can close
    // this connection.
    boost::asio::io_context client_io;
    tcp::socket client(client_io);
    client.connect(
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));

    BOOST_CHECK(
        wait_until([&] { return listener.tls_handshake_failures_total() == 1; },
                   std::chrono::seconds(5)));
    BOOST_CHECK_EQUAL(listener.tls_handshakes_total(), 0u);
    BOOST_CHECK_EQUAL(connects.load(), 0);
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);

    std::uint8_t buf[64];
    boost::system::error_code read_ec;
    client.read_some(boost::asio::buffer(buf), read_ec);
    BOOST_CHECK(read_ec);

    listener.stop();
    io.stop();
    io_thread.join();
}

BOOST_AUTO_TEST_CASE(BlocklistRejectsBeforeHandshake) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<int> connects{0};
    SessionCallbacks callbacks;
    callbacks.create_protocol_pipeline = [] { return make_json_pipeline(); };
    callbacks.on_connect = [&](std::shared_ptr<Session>) { ++connects; };

    TcpListener listener(io, port, std::move(callbacks));
    listener.set_tls(make_fixture_context(), 5000);
    // The loopback address is the peer in this test, so the blocklist hits
    // every accept.
    std::string blocklist_error;
    BOOST_REQUIRE(listener.set_blocklist({"127.0.0.1"}, &blocklist_error));
    listener.start();

    std::thread io_thread([&io] { io.run(); });

    boost::asio::io_context client_io;
    tcp::socket client(client_io);
    boost::system::error_code connect_ec;
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port),
                   connect_ec);
    BOOST_CHECK(!connect_ec);

    BOOST_CHECK(
        wait_until([&] { return listener.blocked_rejects_total() == 1; },
                   std::chrono::seconds(5)));
    // Rejection happened before any handshake work: the TLS counters stay
    // untouched and no session exists.
    BOOST_CHECK_EQUAL(listener.tls_handshakes_total(), 0u);
    BOOST_CHECK_EQUAL(listener.tls_handshake_failures_total(), 0u);
    BOOST_CHECK_EQUAL(connects.load(), 0);
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);

    listener.stop();
    io.stop();
    io_thread.join();
}

// A peer that vanished between handshake completion and adoption: the
// session is built on a dead socket without throwing, then tears itself down
// on the first receive completion.
BOOST_AUTO_TEST_CASE(SessionToleratesDisconnectedPeerAtAdoption) {
    boost::asio::io_context io;
    std::atomic<bool> connected{false};
    std::atomic<bool> disconnected{false};

    SessionCallbacks callbacks;
    callbacks.on_connect = [&](std::shared_ptr<Session>) { connected = true; };
    callbacks.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    // Unconnected socket == "peer already gone" without needing a race.
    auto session = std::make_shared<shield::net::TcpSession>(
        1, boost::asio::ip::tcp::socket(io), std::move(callbacks));
    BOOST_CHECK_EQUAL(session->remote_addr().ip, "");
    BOOST_CHECK_EQUAL(session->remote_addr().port, 0u);
    session->start();
    BOOST_CHECK(connected.load());
    io.run_for(std::chrono::milliseconds(200));
    BOOST_CHECK(disconnected.load());
}

// SessionStream contract on the TLS path: the transport tag is "tls" and the
// wrapped stream is reachable for the listener-side handshake.
BOOST_AUTO_TEST_CASE(TlsStreamTransportTag) {
    boost::asio::io_context io;
    auto ctx = make_fixture_context();
    BOOST_REQUIRE(ctx);
    shield::net::TlsStream stream(tcp::socket(io), *ctx);
    BOOST_CHECK_EQUAL(stream.transport_name(), "tls");

    shield::net::SessionStream& base = stream;
    BOOST_CHECK_EQUAL(base.transport_name(), "tls");
}

BOOST_AUTO_TEST_SUITE_END()
