#define BOOST_TEST_MODULE CovListener
#include <atomic>
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "shield/net/listener.hpp"
#include "shield/transport/protocol.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

using shield::net::ListenerRegistry;
using shield::net::Session;
using shield::net::SessionCallbacks;
using shield::net::SessionId;
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

std::vector<std::uint8_t> bytes(std::string_view s) {
    return {s.begin(), s.end()};
}

// LenPrefix envelope + JSON body routes: route 1001 "login" decodes locally.
// Mirrors the pipeline helper in test_cov_session.cpp so listener-created
// sessions can ingest real frames.
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

std::vector<std::uint8_t> encode_packet(ProtocolPipeline& pipe,
                                        std::string_view body) {
    Packet packet;
    packet.body = bytes(body);
    auto encoded = pipe.encode(packet.ref());
    BOOST_REQUIRE(pipe.error().empty());
    return encoded;
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
    BOOST_CHECK_EQUAL(listener.accepts_total(), 2u);
    BOOST_CHECK_EQUAL(listener.conn_limit_rejects_total(), 1u);
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
    BOOST_CHECK_EQUAL(listener.ip_limit_rejects_total(), 1u);

    c1.close();
    c2.close();
    listener.stop();
    io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(PerIpLimitBelowLimitAllowsConnection) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    listener.set_max_per_ip(2);
    listener.start();

    // Both connections come from 127.0.0.1; the second one finds an existing
    // IP entry below the limit and must be accepted (no rejection recorded).
    Client c1, c2;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(100ms);
    BOOST_REQUIRE(c2.connect(port));
    io.run_for(200ms);
    BOOST_CHECK_EQUAL(listener.session_count(), 2u);
    BOOST_CHECK_EQUAL(listener.last_rejection_reason(), "");

    c1.close();
    c2.close();
    listener.stop();
    io.run_for(100ms);
}

// A blocked address is rejected at accept: no session object is created and
// the listener records the dedicated reason.
BOOST_AUTO_TEST_CASE(BlockedAddressIsRejectedAtAccept) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    // The test client connects over loopback, so deny 127.0.0.1.
    BOOST_REQUIRE(listener.set_blocklist({"127.0.0.1"}));
    listener.start();

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(200ms);

    BOOST_CHECK(wait_until(
        [&] { return listener.last_rejection_reason() == "blocked_ip"; }));
    // Rejected before a session exists: no per-session state was allocated.
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);
    // Counters: one accepted TCP connection, one blocklist rejection.
    BOOST_CHECK_EQUAL(listener.accepts_total(), 1u);
    BOOST_CHECK_EQUAL(listener.blocked_rejects_total(), 1u);
    BOOST_CHECK_EQUAL(listener.conn_limit_rejects_total(), 0u);
    BOOST_CHECK_EQUAL(listener.ip_limit_rejects_total(), 0u);

    c1.close();
    listener.stop();
    io.run_for(100ms);
}

// A CIDR rule blocks the loopback range the test clients come from.
BOOST_AUTO_TEST_CASE(BlockedCidrRejectsAtAccept) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    BOOST_REQUIRE(listener.set_blocklist({"127.0.0.0/8"}));
    listener.start();

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(200ms);

    BOOST_CHECK(wait_until(
        [&] { return listener.last_rejection_reason() == "blocked_ip"; }));
    BOOST_CHECK_EQUAL(listener.session_count(), 0u);

    c1.close();
    listener.stop();
    io.run_for(100ms);
}

// A blocklist that does not cover the peer leaves the connection alone.
BOOST_AUTO_TEST_CASE(UnrelatedBlocklistAllowsConnection) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    // Documentation-range addresses: never the loopback test client.
    BOOST_REQUIRE(listener.set_blocklist({"203.0.113.7", "2001:db8::/32"}));
    listener.start();

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(200ms);

    BOOST_CHECK_EQUAL(listener.session_count(), 1u);
    BOOST_CHECK_EQUAL(listener.last_rejection_reason(), "");

    c1.close();
    listener.stop();
    io.run_for(100ms);
}

// A malformed rule set is refused outright and the previous one survives, so
// a bad edit cannot silently disable an in-force blocklist.
BOOST_AUTO_TEST_CASE(BlocklistSetFailureKeepsPreviousRules) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    SessionCallbacks callbacks;
    TcpListener listener(io, port, callbacks);
    BOOST_REQUIRE(listener.set_blocklist({"127.0.0.1"}));

    std::string error;
    BOOST_CHECK(!listener.set_blocklist({"198.51.100.0/24", "bogus"}, &error));
    BOOST_CHECK(error.find("not an IP address") != std::string::npos);

    // The original 127.0.0.1 rule is still the one in force.
    listener.start();
    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(200ms);
    BOOST_CHECK(wait_until(
        [&] { return listener.last_rejection_reason() == "blocked_ip"; }));

    c1.close();
    listener.stop();
    io.run_for(100ms);
}

// End to end through a listener-created session: over-budget frames are
// dropped before dispatch, charge the listener's cumulative counter, and
// fire the chained user callback — exactly once per dropped frame.
BOOST_AUTO_TEST_CASE(ListenerChainsRateLimitedCallback) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<int> packets{0};
    std::atomic<int> drop_callbacks{0};
    std::atomic<bool> disconnected{false};

    SessionCallbacks callbacks;
    callbacks.create_protocol_pipeline = [] { return make_json_pipeline(); };
    callbacks.on_packet = [&](std::shared_ptr<Session>, const DispatchResult&) {
        ++packets;
    };
    callbacks.on_rate_limited = [&] { ++drop_callbacks; };
    callbacks.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    TcpListener listener(io, port, callbacks);
    // 1/s sustained with burst 5: the first five frames pass, the rest are
    // dropped. Refill is lazy at 1 token/s, and all 8 frames drain from one
    // TCP segment in well under that, so the split is deterministic.
    listener.set_rate_limit(1, 5);
    listener.start();

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(150ms);
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    auto local = make_json_pipeline();
    std::vector<std::uint8_t> wire;
    for (int i = 0; i < 8; ++i) {
        auto f = encode_packet(*local, R"({"route":"login","payload":{"seq":)" +
                                           std::to_string(i) + "}}");
        wire.insert(wire.end(), f.begin(), f.end());
    }
    c1.send(wire);
    io.run_for(200ms);

    BOOST_CHECK_EQUAL(packets.load(), 5);
    BOOST_CHECK_EQUAL(drop_callbacks.load(), 3);
    BOOST_CHECK_EQUAL(listener.rate_limited_messages_total(), 3u);
    // Drops are not fatal: the session stays on the listener.
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);
    BOOST_CHECK(!disconnected.load());

    c1.close();
    listener.stop();
    io.run_for(100ms);
}

// Same drop path with no user-facing on_rate_limited: the wrapper still
// charges the listener's cumulative counter (the skip-user-callback arm).
BOOST_AUTO_TEST_CASE(ListenerCounterWithoutUserRateLimitedCallback) {
    boost::asio::io_context io;
    const auto port = reserve_ephemeral_port(io);

    std::atomic<int> packets{0};
    SessionCallbacks callbacks;
    callbacks.create_protocol_pipeline = [] { return make_json_pipeline(); };
    callbacks.on_packet = [&](std::shared_ptr<Session>, const DispatchResult&) {
        ++packets;
    };
    // Deliberately no callbacks.on_rate_limited.

    TcpListener listener(io, port, callbacks);
    listener.set_rate_limit(1, 5);
    listener.start();

    Client c1;
    BOOST_REQUIRE(c1.connect(port));
    io.run_for(150ms);
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    auto local = make_json_pipeline();
    std::vector<std::uint8_t> wire;
    for (int i = 0; i < 8; ++i) {
        auto f = encode_packet(*local, R"({"route":"login","payload":{"seq":)" +
                                           std::to_string(i) + "}}");
        wire.insert(wire.end(), f.begin(), f.end());
    }
    c1.send(wire);
    io.run_for(200ms);

    BOOST_CHECK_EQUAL(packets.load(), 5);
    BOOST_CHECK_EQUAL(listener.rate_limited_messages_total(), 3u);
    BOOST_CHECK_EQUAL(listener.session_count(), 1u);

    c1.close();
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

// Listening listeners register for the /ops/metrics gateway view; a failed
// bind never registers, and destruction unregisters.
BOOST_AUTO_TEST_CASE(ListenerRegistryLifecycle) {
    const auto present = [](uint16_t port) {
        for (const auto* l : ListenerRegistry::instance().snapshot()) {
            if (l->port() == port) return true;
        }
        return false;
    };

    boost::asio::io_context io;
    const auto bound_port = reserve_ephemeral_port(io);
    TcpListener listening(io, bound_port, {});
    BOOST_CHECK(present(bound_port));

    // A listener whose bind fails never enters the registry.
    const auto held_port = reserve_ephemeral_port(io);
    tcp::acceptor holder(io, tcp::endpoint(tcp::v4(), held_port));
    TcpListener failed(io, held_port, {});
    BOOST_CHECK(!failed.is_open());
    BOOST_CHECK(!present(held_port));

    // Destruction unregisters.
    const auto scoped_port = reserve_ephemeral_port(io);
    {
        TcpListener scoped(io, scoped_port, {});
        BOOST_CHECK(present(scoped_port));
    }
    BOOST_CHECK(!present(scoped_port));

    BOOST_CHECK(present(bound_port));
}

BOOST_AUTO_TEST_SUITE_END()
