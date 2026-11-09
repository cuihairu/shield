// [SHIELD_CLUSTER] cluster_transport — internal cross-TU
// declarations for the source split.
//
// Not a public contract: the public API stays in
// shield/cluster/cluster_transport.hpp. The peer/transport state
// structs, the message-loop state and the facade's Impl move here
// verbatim so the cluster_transport*.cpp translation units share
// one layout; the file-local helpers shared across the split are
// defined flat (external linkage) in their own TU, one declaration
// each.
#pragma once

#include <atomic>
#include <caf/actor.hpp>
#include <caf/behavior.hpp>
#include <caf/stateful_actor.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shield/base/time.hpp"
#include "shield/cluster/cluster_auth.hpp"
#include "shield/cluster/cluster_messages.hpp"
#include "shield/cluster/cluster_transport.hpp"

namespace shield::cluster {

// Loop cadence floor so a misconfigured 0ms heartbeat cannot busy-loop.
constexpr int kMinTickMs = 50;

// Upper bound on a single dial. The middleman's blocking remote_actor()
// waits forever, which would freeze the connect loop when a peer restarts
// mid-dial, so we talk to the middleman actor directly with a timeout.
constexpr auto kDialTimeout = std::chrono::seconds(2);

// Grace past the caller-side call timeout before a peer-binding entry is
// reclaimed: covers scheduling and network delay so a legit reply that is
// about to reach the caller-side timeout is still routable.
constexpr int64_t kReplyGraceMs = 5000;
// Caller-side default when timeout_ms <= 0 (mirrors LuaServiceManager's
// suspend_for_call, which treats a non-positive timeout as 5000 ms).
constexpr int32_t kDefaultCallTimeoutMs = 5000;

struct PeerLink {
    std::string address;  // dial target, "host:port"
    std::string host;
    uint16_t port = 0;
    std::string node_id;  // announced identity, learned at handshake
    caf::actor handle;    // invalid until the dial succeeds
    HelloMsg hello;
    shield::base::TimePoint handshake_deadline{};
    bool dialing = false;  // a dial request is in flight
    // Set when a live connection to this peer dropped; the next successful
    // dial is then a reconnect (M5 counter), not the first connect.
    bool dropped = false;
};

// State shared between the transport actor and the ClusterTransport facade
// methods (send_envelope / complete_proxied_call), which run on foreign
// threads. Shared ownership keeps the actor's captures valid regardless of
// teardown ordering.
struct TransportSideState {
    std::shared_mutex peers_mutex;
    // Adopted node_id -> live connection handle (M4 data plane). Both nodes
    // dial each other, so the dialer-side adoption fills this on both ends.
    std::unordered_map<std::string, caf::actor> peers_by_node;
    caf::actor outbound_actor;
    // Sender handles observed on inbound Hello messages. A peer can send its
    // data-plane traffic over the connection it initiated, which is distinct
    // from our own outbound connection to that same peer.
    std::vector<std::pair<caf::actor, std::string>> inbound_peer_senders;
    // Guards proxied_calls AND bridges: bridges are installed on a foreign
    // thread and cleared by stop() while the actor keeps dispatching.
    std::mutex data_mutex;
    // Proxied call sessions (M4): local session -> (remote call_session,
    // source node). Filled when an envelope call is dispatched, drained by
    // complete_proxied_call or callee-side expiry.
    std::unordered_map<uint64_t, std::pair<uint64_t, std::string>>
        proxied_calls;
    // Caller-side peer binding for proxied calls: call_session -> (target
    // node, deadline). An inbound envelope reply is only routed to the
    // pending call when the reply sender's Hello identity matches the node
    // the call was actually sent to; a forged reply from any other sender is
    // dropped. Without this, a client that can reach the cluster port can
    // guess the sequential call_session and inject a reply, since the old
    // check only ran when cluster auth was enabled. Entries are reclaimed
    // lazily once the caller-side timeout (plus a grace margin) lapses.
    struct OutboundBinding {
        std::string target_node;
        int64_t deadline_ms = 0;
    };
    std::unordered_map<uint64_t, OutboundBinding> outbound_sessions;
    EnvelopeBridges bridges;
    // M5 observability counters. Touched from the actor thread and the
    // facade threads; admin endpoints read them without either mutex.
    // "Messages" are data-plane envelopes and replies, counted when they
    // actually leave / when they arrive; route-table syncs ride with
    // heartbeats and are not counted.
    std::atomic<uint64_t> tx_messages{0};
    std::atomic<uint64_t> rx_messages{0};
    std::atomic<uint64_t> tx_heartbeats{0};
    std::atomic<uint64_t> rx_heartbeats{0};
    std::atomic<uint64_t> reconnects{0};
};

using SidePtr = std::shared_ptr<TransportSideState>;

struct PendingHandshake {
    caf::actor sender;
    HelloMsg hello;
    HelloAckMsg ack;
    shield::base::TimePoint deadline;
};

struct transport_state {
    ClusterManager* manager = nullptr;
    std::string self_node_id;
    uint64_t self_epoch = 0;
    int heartbeat_interval_ms = 5000;
    std::vector<PeerLink> peers;
    uint64_t hb_seq = 0;
    ClusterAuthConfig auth;
    std::vector<PendingHandshake> pending;
    std::unordered_map<std::string, shield::base::TimePoint> seen_nonces;
};

using transport_actor = caf::stateful_actor<transport_state>;

struct ClusterTransport::Impl {
    caf::actor_system* system = nullptr;
    ClusterManager* manager = nullptr;
    ClusterConfig config;
    uint16_t bound_port = 0;
    bool running = false;
    // Shared with the actor and with the facade methods below, which run on
    // whatever thread calls send_envelope / complete_proxied_call.
    SidePtr side = std::make_shared<TransportSideState>();
};

// Defined in cluster_transport_loop.cpp: the published transport
// actor (start() spawns it with the facade's manager/config/side).
caf::behavior transport_loop(transport_actor* self, ClusterManager* manager,
                             const ClusterConfig cfg, const SidePtr& side);

// Defined in cluster_transport_loop.cpp: routes an envelope reply to
// the node a call envelope came from (used by
// complete_proxied_call).
void reply_to(const SidePtr& side, const std::string& source_node,
              const EnvelopeReplyMsg& reply);

}  // namespace shield::cluster
