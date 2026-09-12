#define BOOST_TEST_MODULE CovSession
#include <atomic>
#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "shield/net/session.hpp"
#include "shield/transport/protocol.hpp"

namespace {

using boost::asio::ip::tcp;
using namespace std::chrono_literals;

using shield::net::Session;
using shield::net::SessionCallbacks;
using shield::net::SessionId;
using shield::net::TcpSession;
using shield::transport::BodyCodec;
using shield::transport::BodyCodecRegistry;
using shield::transport::BodyRouteKey;
using shield::transport::DecodedBody;
using shield::transport::DispatchResult;
using shield::transport::EnvelopeKind;
using shield::transport::JsonBodyCodec;
using shield::transport::Packet;
using shield::transport::PacketRef;
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

// Accepted server-side socket + connected client socket on loopback. Port is
// assigned by the OS (acceptor binds to port 0).
struct SocketPair {
    boost::asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint(tcp::v4(), 0)};
    tcp::socket server{io};
    tcp::socket client{io};

    SocketPair() {
        const auto port = acceptor.local_endpoint().port();
        acceptor.async_accept(server, [](const boost::system::error_code&) {});
        client.connect(
            tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
        io.run();
        // run() exhausted the work queue and left the context stopped;
        // un-stop it so later run_for() calls actually poll.
        io.restart();
    }
};

// Read exactly n bytes from the client side; empty vector on timeout/error.
std::vector<std::uint8_t> read_exact(tcp::socket& sock,
                                     boost::asio::io_context& io, size_t n) {
    std::vector<std::uint8_t> out(n);
    boost::system::error_code ec;
    boost::asio::async_read(
        sock, boost::asio::buffer(out),
        [&](const boost::system::error_code& e, size_t) { ec = e; });
    io.run_for(2s);
    io.stop();
    io.restart();
    if (ec) out.clear();
    return out;
}

// LenPrefix envelope + JSON body routes: 1001 "login" DecodeLocal(lazy),
// 1002 "fwd" ForwardRaw. Unknown routes are dropped.
std::unique_ptr<ProtocolPipeline> make_json_pipeline(
    size_t envelope_max_frame = 0) {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 1001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .debug_name = "login",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = true},
    });
    routes.add(RouteEntry{
        .route_id = 1002,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .debug_name = "fwd",
        .policy =
            RoutePolicy{.action = RouteAction::ForwardRaw, .lazy_decode = true},
    });

    BodyCodecRegistry codecs;
    codecs.add(1, std::make_unique<JsonBodyCodec>());

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.envelope.max_frame_size = envelope_max_frame;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    profile.unknown_route_action = RouteAction::Drop;

    return std::make_unique<ProtocolPipeline>(
        std::move(profile), std::move(routes), std::move(codecs));
}

// Body codec whose decode() always throws; used to force a lazy-decode
// materialize failure inside the session strand.
class ThrowingCodec final : public BodyCodec {
public:
    std::string_view name() const override { return "throwing"; }

    std::optional<BodyRouteKey> route_key(PacketRef) override {
        BodyRouteKey key;
        key.route_name = "boom";
        return key;
    }

    DecodedBody decode(PacketRef, const RouteEntry&) override {
        throw std::runtime_error("kaboom");
    }

    std::vector<std::uint8_t> encode(const DecodedBody&, const RouteEntry&,
                                     const ProtocolProfile&) override {
        return {};
    }
};

std::unique_ptr<ProtocolPipeline> make_throwing_pipeline() {
    RouteTable routes;
    routes.add(RouteEntry{
        .route_id = 2001,
        .direction = RouteDirection::ClientToServer,
        .codec_id = 1,
        .debug_name = "boom",
        .policy = RoutePolicy{.action = RouteAction::DecodeLocal,
                              .lazy_decode = true},
    });
    BodyCodecRegistry codecs;
    codecs.add(1, std::make_unique<ThrowingCodec>());

    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 1;
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    return std::make_unique<ProtocolPipeline>(
        std::move(profile), std::move(routes), std::move(codecs));
}

// Body-route pipeline whose default codec is not registered: every packet
// resolves to a dispatch error result.
std::unique_ptr<ProtocolPipeline> make_broken_pipeline() {
    ProtocolProfile profile;
    profile.envelope_kind = EnvelopeKind::LenPrefix;
    profile.default_codec_id = 7;  // not registered
    profile.route_source = RouteSource::Body;
    profile.decode_body_route = true;
    RouteTable routes;
    BodyCodecRegistry codecs;  // empty
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

void write_client(tcp::socket& sock, const std::vector<std::uint8_t>& data) {
    boost::system::error_code ec;
    boost::asio::write(sock, boost::asio::buffer(data), ec);
    BOOST_REQUIRE(!ec);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(SessionCoverage)

BOOST_AUTO_TEST_CASE(LifecycleAndMetadataAccessors) {
    SocketPair p;

    bool connected = false;
    std::atomic<bool> disconnected{false};
    std::string disconnect_reason;

    SessionCallbacks cbs;
    cbs.on_connect = [&](std::shared_ptr<Session>) { connected = true; };
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view reason) {
        disconnect_reason = std::string(reason);
        disconnected = true;
    };

    auto session = std::make_shared<TcpSession>(7, std::move(p.server), cbs);
    BOOST_CHECK_EQUAL(session->id(), 7u);
    BOOST_CHECK(session->is_alive());
    BOOST_CHECK_EQUAL(session->error_code(), "");
    BOOST_CHECK(!session->has_protocol_pipeline());
    BOOST_CHECK(session->protocol_codec_name().empty());
    BOOST_CHECK_EQUAL(session->remote_addr().ip, "127.0.0.1");
    BOOST_CHECK_GT(session->remote_addr().port, 0u);
    BOOST_CHECK(!session->remote_addr().to_string().empty());

    // user data
    session->set_user_data("k1", "v1");
    BOOST_CHECK_EQUAL(session->get_user_data("k1"), "v1");
    BOOST_CHECK_EQUAL(session->get_user_data("missing"), "");

    // Fresh session: an all-empty binding at epoch 0.
    shield::net::SessionBinding fresh = session->binding();
    BOOST_CHECK_EQUAL(fresh.target_service, "");
    BOOST_CHECK_EQUAL(fresh.player_id, "");
    BOOST_CHECK_EQUAL(fresh.gateway_name, "");
    BOOST_CHECK_EQUAL(fresh.protocol_profile_id, "");
    BOOST_CHECK_EQUAL(fresh.epoch, 0u);

    // reset_binding installs the pre-auth identity in one shot.
    session->reset_binding({"AuthService", "", "gw:1", "profile-A", 0});
    fresh = session->binding();
    BOOST_CHECK_EQUAL(fresh.target_service, "AuthService");
    BOOST_CHECK_EQUAL(fresh.gateway_name, "gw:1");
    BOOST_CHECK_EQUAL(fresh.protocol_profile_id, "profile-A");
    BOOST_CHECK_EQUAL(fresh.epoch, 0u);
    BOOST_CHECK_EQUAL(fresh.player_id, "");

    // CAS read: a stale expected epoch is rejected and leaves the binding
    // untouched.
    shield::net::SessionBinding out;
    BOOST_CHECK(!session->apply_binding("PlayerService", "player-9", 5, &out));
    BOOST_CHECK_EQUAL(session->binding().target_service, "AuthService");
    BOOST_CHECK_EQUAL(session->binding().epoch, 0u);
    BOOST_CHECK_EQUAL(out.epoch, 0u);  // out only written on success

    // CAS hit: matching expected epoch flips target+player and bumps epoch.
    BOOST_CHECK(session->apply_binding("PlayerService", "player-9", 0, &out));
    BOOST_CHECK_EQUAL(out.target_service, "PlayerService");
    BOOST_CHECK_EQUAL(out.player_id, "player-9");
    BOOST_CHECK_EQUAL(out.epoch, 1u);
    BOOST_CHECK_EQUAL(session->binding().epoch, 1u);
    BOOST_CHECK_EQUAL(session->binding().target_service, "PlayerService");

    // kAnyEpoch bypasses the staleness check (gateway invalidation path).
    BOOST_CHECK(session->apply_binding("", "", shield::net::kAnyEpoch, &out));
    BOOST_CHECK_EQUAL(out.target_service, "");
    BOOST_CHECK_EQUAL(out.player_id, "");
    BOOST_CHECK_EQUAL(out.epoch, 2u);

    // start fires on_connect
    session->start();
    p.io.run_for(100ms);
    BOOST_CHECK(connected);

    // send on a closed session fails synchronously
    session->close("normal");
    session->close("normal");  // second close is a no-op
    p.io.run_for(100ms);
    BOOST_CHECK(disconnected);
    BOOST_CHECK_EQUAL(disconnect_reason, "normal");
    BOOST_CHECK(!session->is_alive());

    std::string err;
    BOOST_CHECK(!session->send(bytes("x"), &err));
    BOOST_CHECK_EQUAL(err, "session is closed");
    err.clear();
    DecodedBody msg;
    BOOST_CHECK(!session->send_message(msg, &err));
    BOOST_CHECK_EQUAL(err, "session is closed");
}

BOOST_AUTO_TEST_CASE(RawIngressWithoutPipelineIsRejected) {
    SocketPair p;

    std::atomic<bool> disconnected{false};
    SessionCallbacks cbs;
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    auto session = std::make_shared<TcpSession>(1, std::move(p.server), cbs);
    session->start();
    BOOST_CHECK(!session->has_protocol_pipeline());

    // Empty send is accepted without queuing anything.
    std::string err;
    BOOST_CHECK(session->send({}, &err));
    BOOST_CHECK(err.empty());

    // Server -> client raw send still works before any ingress.
    BOOST_CHECK(session->send(bytes("pong")));
    auto got = read_exact(p.client, p.io, 4);
    BOOST_CHECK_EQUAL(got.size(), 4u);
    BOOST_CHECK(std::string(got.begin(), got.end()) == "pong");
    BOOST_CHECK(session->is_alive());

    // Raw-byte ingress without a protocol pipeline is rejected: the old
    // fallback silently dropped everything, so this closes the session with
    // a stable error code instead.
    write_client(p.client, bytes("raw bytes"));
    p.io.run_for(200ms);
    BOOST_CHECK(disconnected.load());
    BOOST_CHECK(!session->is_alive());
    BOOST_CHECK_EQUAL(session->error_code(), "protocol_not_configured");
}

BOOST_AUTO_TEST_CASE(ReadIdleTimeoutClosesSession) {
    SocketPair p;

    std::atomic<bool> disconnected{false};
    SessionCallbacks cbs;
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    auto session =
        std::make_shared<TcpSession>(3, std::move(p.server), cbs, 0, 0, 60);
    session->start();

    // Stay silent past the idle window: deadline must fire and close the
    // session. (Inbound data would end the session via the pipeline check,
    // so keepalive traffic cannot be exercised here without a pipeline.)
    p.io.run_for(500ms);
    BOOST_CHECK(disconnected.load());
    BOOST_CHECK(!session->is_alive());
#ifndef _WIN32
    // On Windows the socket EOF frequently wins the race against the idle
    // deadline, mapping the close to session_closed instead.
    BOOST_CHECK_EQUAL(session->error_code(), "read_idle_timeout");
#endif
}

BOOST_AUTO_TEST_CASE(ClientEofClosesSession) {
    SocketPair p;

    std::atomic<bool> disconnected{false};
    SessionCallbacks cbs;
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    auto session = std::make_shared<TcpSession>(4, std::move(p.server), cbs);
    session->start();
    p.io.run_for(50ms);

    p.client.shutdown(tcp::socket::shutdown_both);
    p.io.run_for(200ms);

    BOOST_CHECK(disconnected.load());
    BOOST_CHECK(!session->is_alive());
    BOOST_CHECK_EQUAL(session->error_code(), "session_closed");
}

BOOST_AUTO_TEST_CASE(SendQueueBackpressure) {
    SocketPair p;

    SessionCallbacks cbs;
    auto session =
        std::make_shared<TcpSession>(5, std::move(p.server), cbs, 0, 2);

    // Keep the io_context idle so accepted sends stay queued on the strand.
    std::string err;
    BOOST_CHECK(session->send(bytes("a"), &err));
    BOOST_CHECK(session->send(bytes("b"), &err));
    BOOST_CHECK(!session->send(bytes("c"), &err));
    BOOST_CHECK_EQUAL(err, "session_send_queue_full");
    BOOST_CHECK(session->send({}, &err));  // empty still fine

    // Closing before the strand drains makes the posted sends roll their
    // reservations back, and close() drops whatever was queued.
    session->close("backpressure");
    p.io.run_for(150ms);
    BOOST_CHECK(!session->is_alive());
}

BOOST_AUTO_TEST_CASE(SendMessageBackpressureAndRollback) {
    SocketPair p;

    SessionCallbacks cbs;
    cbs.create_protocol_pipeline = [] { return make_json_pipeline(); };
    auto session =
        std::make_shared<TcpSession>(6, std::move(p.server), cbs, 0, 1);

    std::string err;
    BOOST_CHECK(session->send(bytes("occupy"), &err));
    DecodedBody msg;
    msg.route_id = 1001;
    BOOST_CHECK(!session->send_message(msg, &err));
    BOOST_CHECK_EQUAL(err, "session_send_queue_full");
    session->close("full");
    p.io.run_for(150ms);
    BOOST_CHECK(!session->is_alive());

    // Rollback path: send_message accepted, then the session dies before the
    // strand drains the encode post.
    SocketPair p2;
    SessionCallbacks cbs2;
    cbs2.create_protocol_pipeline = [] { return make_json_pipeline(); };
    auto session2 =
        std::make_shared<TcpSession>(7, std::move(p2.server), cbs2, 0, 4);
    BOOST_CHECK(session2->send_message(msg, &err));
    session2->close("rollback");
    p2.io.run_for(150ms);
    BOOST_CHECK(!session2->is_alive());
}

BOOST_AUTO_TEST_CASE(SendMessageWithoutPipelineFails) {
    SocketPair p;
    SessionCallbacks cbs;  // no create_protocol_pipeline
    auto session = std::make_shared<TcpSession>(8, std::move(p.server), cbs);

    std::string err;
    DecodedBody msg;
    BOOST_CHECK(!session->send_message(msg, &err));
    BOOST_CHECK_EQUAL(err, "protocol pipeline is not configured");
    session->close("normal");
    p.io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(CloseAbortsInFlightWriteAndDropsQueue) {
    SocketPair p;
    SessionCallbacks cbs;
    auto session = std::make_shared<TcpSession>(9, std::move(p.server), cbs);

    session->start();
    // Big payload + a trailing message: the 8 MiB write stays in flight
    // (client never reads) and the second message stays queued.
    std::vector<std::uint8_t> big(8u * 1024 * 1024, 'x');
    BOOST_CHECK(session->send(big));
    BOOST_CHECK(session->send(bytes("tail")));
    p.io.run_for(80ms);
    BOOST_CHECK(session->is_alive());

    session->close("mid-write");
    p.io.run_for(200ms);
    BOOST_CHECK(!session->is_alive());
}

BOOST_AUTO_TEST_CASE(WriteErrorOnClosedPeerClosesSession) {
    SocketPair p;
    SessionCallbacks cbs;
    // No start(): without a pending read there is no EOF race, so the async
    // write is guaranteed to observe the dead peer while still alive.
    auto session = std::make_shared<TcpSession>(10, std::move(p.server), cbs);
    // SO_LINGER(0) + close sends RST instead of FIN: the next write on the
    // server side fails with EPIPE/ECONNRESET. Wait (wall clock) for the RST
    // to arrive before the first write is issued.
    p.client.set_option(tcp::socket::linger(true, 0));
    p.client.close();
    std::this_thread::sleep_for(100ms);

    for (int i = 0; i < 10 && session->is_alive(); ++i) {
        session->send(bytes("flush"));
        p.io.run_for(100ms);
    }
    BOOST_CHECK(!session->is_alive());
    BOOST_CHECK_EQUAL(session->error_code(), "session_closed");
}

BOOST_AUTO_TEST_CASE(PipelineDispatchPaths) {
    SocketPair p;

    std::atomic<int> packets{0};
    std::atomic<int> decoded_count{0};
    std::atomic<int> forwarded_count{0};

    SessionCallbacks cbs;
    cbs.create_protocol_pipeline = [] { return make_json_pipeline(); };
    cbs.on_packet = [&](std::shared_ptr<Session>, const DispatchResult& r) {
        ++packets;
        if (r.should_forward_raw()) {
            ++forwarded_count;
        } else if (r.decoded()) {
            ++decoded_count;
        }
    };

    auto session = std::make_shared<TcpSession>(11, std::move(p.server), cbs);
    session->start();
    BOOST_CHECK(session->has_protocol_pipeline());
    BOOST_CHECK_EQUAL(session->protocol_codec_name(), "json");

    // Three frames in one TCP segment: decode-local (lazy materialize),
    // forward-raw and an unknown route that is dropped.
    auto local = make_json_pipeline();
    std::vector<std::uint8_t> wire;
    auto a = encode_packet(*local, R"({"route":"login","payload":{"uid":7}})");
    auto b = encode_packet(*local, R"({"route":"fwd","payload":{"x":1}})");
    auto c = encode_packet(*local, R"({"route":"unknown-route"})");
    wire.insert(wire.end(), a.begin(), a.end());
    wire.insert(wire.end(), b.begin(), b.end());
    wire.insert(wire.end(), c.begin(), c.end());
    write_client(p.client, wire);
    p.io.run_for(200ms);

    BOOST_CHECK_EQUAL(packets.load(), 2);
    BOOST_CHECK_EQUAL(decoded_count.load(), 1);
    BOOST_CHECK_EQUAL(forwarded_count.load(), 1);
    BOOST_CHECK(session->is_alive());

    session->close("normal");
    p.io.run_for(100ms);
}

BOOST_AUTO_TEST_CASE(PipelineEnvelopeErrorClosesSession) {
    SocketPair p;

    std::atomic<bool> disconnected{false};
    SessionCallbacks cbs;
    cbs.create_protocol_pipeline = [] { return make_json_pipeline(16); };
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    auto session = std::make_shared<TcpSession>(12, std::move(p.server), cbs);
    session->start();

    auto local = make_json_pipeline(16);
    // Body far larger than the envelope's 16 byte limit.
    std::string big(256, 'z');
    write_client(p.client, encode_packet(*local, big));
    p.io.run_for(200ms);

    BOOST_CHECK(disconnected.load());
    BOOST_CHECK_EQUAL(session->error_code(), "decode_error");
}

BOOST_AUTO_TEST_CASE(PipelineDispatchErrorClosesSession) {
    SocketPair p;

    std::atomic<bool> disconnected{false};
    SessionCallbacks cbs;
    cbs.create_protocol_pipeline = [] { return make_broken_pipeline(); };
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    auto session = std::make_shared<TcpSession>(13, std::move(p.server), cbs);
    session->start();

    ProtocolProfile scratch;
    scratch.envelope_kind = EnvelopeKind::LenPrefix;
    RouteTable empty_routes;
    BodyCodecRegistry no_codecs;
    ProtocolPipeline encoder(std::move(scratch), std::move(empty_routes),
                             std::move(no_codecs));
    write_client(p.client, encode_packet(encoder, R"({"route":"anything"})"));
    p.io.run_for(200ms);

    // Dispatch errors carry the generic session_closed error code (the
    // reason string "protocol dispatch error" matches no decode branch).
    BOOST_CHECK(disconnected.load());
    BOOST_CHECK_EQUAL(session->error_code(), "session_closed");
}

BOOST_AUTO_TEST_CASE(PipelineMaterializeFailureClosesSession) {
    SocketPair p;

    std::atomic<bool> disconnected{false};
    SessionCallbacks cbs;
    cbs.create_protocol_pipeline = [] { return make_throwing_pipeline(); };
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {
        disconnected = true;
    };

    auto session = std::make_shared<TcpSession>(14, std::move(p.server), cbs);
    session->start();

    auto local = make_throwing_pipeline();
    write_client(p.client, encode_packet(*local, R"({"route":"boom"})"));
    p.io.run_for(200ms);

    BOOST_CHECK(disconnected.load());
    BOOST_CHECK_EQUAL(session->error_code(), "decode_error");
}

BOOST_AUTO_TEST_CASE(SendMessageEncodesThroughPipeline) {
    SocketPair p;

    std::atomic<int> packets{0};
    SessionCallbacks cbs;
    cbs.create_protocol_pipeline = [] { return make_json_pipeline(); };
    cbs.on_packet = [&](std::shared_ptr<Session>, const DispatchResult&) {
        ++packets;
    };
    cbs.on_disconnect = [&](std::shared_ptr<Session>, std::string_view) {};

    auto session = std::make_shared<TcpSession>(15, std::move(p.server), cbs);
    session->start();

    auto local = make_json_pipeline();
    DecodedBody msg;
    msg.route_id = 1001;
    msg.message = nlohmann::json{{"uid", 9}};
    const auto expected = local->encode_message(msg);
    BOOST_REQUIRE(!expected.empty());

    std::string err;
    BOOST_CHECK(session->send_message(msg, &err));
    BOOST_CHECK(err.empty());

    auto got = read_exact(p.client, p.io, expected.size());
    BOOST_CHECK_EQUAL_COLLECTIONS(got.begin(), got.end(), expected.begin(),
                                  expected.end());

    // Encode failure (unknown outbound route) logs and drops the message but
    // keeps the session alive.
    DecodedBody bad;
    bad.route_id = 424242;
    BOOST_CHECK(session->send_message(bad, &err));
    p.io.run_for(200ms);
    BOOST_CHECK(session->is_alive());
    BOOST_CHECK_EQUAL(p.client.available(), 0u);

    session->close("normal");
    p.io.run_for(100ms);
    BOOST_CHECK_EQUAL(packets.load(), 0);
}

BOOST_AUTO_TEST_CASE(SendMessageEmptyBodyEncodesAsEmptyFrame) {
    SocketPair p;

    SessionCallbacks cbs;
    // ThrowingCodec::encode returns an empty body buffer without an error:
    // the envelope still emits a bare length header, so an empty frame goes
    // out and the session stays alive.
    cbs.create_protocol_pipeline = [] { return make_throwing_pipeline(); };

    auto session = std::make_shared<TcpSession>(16, std::move(p.server), cbs);
    session->start();

    DecodedBody msg;
    msg.route_id = 2001;
    std::string err;
    BOOST_CHECK(session->send_message(msg, &err));
    p.io.run_for(200ms);
    BOOST_CHECK(session->is_alive());
    // Bare LenPrefix header for a zero-length body.
    BOOST_CHECK_EQUAL(p.client.available(), 4u);

    session->close("normal");
    p.io.run_for(100ms);
}

BOOST_AUTO_TEST_SUITE_END()
