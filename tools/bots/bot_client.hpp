// tools/bots/bot_client.hpp - wire-level bot client for the shield gateway.
//
// Wire contract (docs/gateway.md + examples/hello_world/config/app.yaml,
// verified by tests/acceptance/test_client_rpc_e2e.cpp):
//   idlen envelope, big endian: [route_id:2B][payload_length:2B][body]
//   body is pure business JSON; the route never travels in it.
//
// Session model: TCP connect lands on the auth entry service (pre-auth);
// a login c2s RPC triggers shield.client.bind, which atomically flips the
// session target to the player service; subsequent c2s routes go there.
// Replies are s2c frames on the same connection, correlated per route
// (fire-and-forget, no sequence numbers — the correlation contract is
// "one reply frame per request frame, in send order").
#ifndef SHIELD_TOOLS_BOTS_BOT_CLIENT_HPP
#define SHIELD_TOOLS_BOTS_BOT_CLIENT_HPP

#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace shield::bots {

/// One decoded wire frame.
struct Frame {
    std::uint32_t route_id = 0;
    std::string body;
};

/// [route_id:2B BE][length:2B BE][body] — the hello_world listener profile.
std::vector<std::uint8_t> encode_frame(std::uint32_t route_id,
                                       const std::string& body);

struct LoginConfig {
    std::uint32_t route_id = 1;
    std::uint32_t reply_route_id = 100;
    std::string player_id;  // the "player_id" JSON field value
};

/// Blocking single-connection client. One instance per bot connection;
/// instances are not thread-safe, but independent instances run on their
/// own threads (the load runner uses one thread per connection).
class BotClient {
public:
    explicit BotClient(std::string target);

    /// Blocking connect with exponential backoff (base 100ms, doubling,
    /// capped at 2s). Returns false when every attempt failed.
    bool connect_with_backoff(int max_attempts);

    /// Send the login route, await the reply route, parse the body.
    /// Returns the reply body, or nullopt on timeout / disconnect.
    std::optional<std::string> login(const LoginConfig& login,
                                     std::chrono::milliseconds timeout,
                                     double* latency_ms = nullptr);

    /// Fire-and-forget c2s frame. Returns false on write error (the
    /// connection is dead afterwards).
    bool send(std::uint32_t route_id, const std::string& body);

    /// Read whatever arrived and return the next complete frame, or
    /// nullopt on timeout / disconnect. Frames arrive in wire order;
    /// poll_frame hands them out one at a time from an internal queue.
    std::optional<Frame> poll_frame(std::chrono::milliseconds timeout);

    /// True while the socket is open and no read/write error was seen.
    bool alive() const { return alive_; }

    void close();

    const std::string& target() const { return target_; }

private:
    bool read_some(std::chrono::milliseconds budget);

    std::string target_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::socket socket_;
    std::vector<std::uint8_t> inbox_;
    std::deque<Frame> pending_;
    bool alive_ = false;
};

/// Single login + move round trip against the hello_world closure.
/// Used by `bots probe` and as the "server still alive" check between
/// chaos injections. Returns false when any leg fails.
struct ProbeResult {
    bool ok = false;
    double login_ms = 0.0;
    double move_ms = 0.0;
    std::string detail;
};
ProbeResult probe_hello_world(const std::string& target,
                              const std::string& player_id,
                              std::chrono::milliseconds timeout);

}  // namespace shield::bots

#endif  // SHIELD_TOOLS_BOTS_BOT_CLIENT_HPP
