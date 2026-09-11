// [SHIELD_CLUSTER] ClusterTransport implementation (M2).
//
// Shape: one published stateful actor handles inbound hello/heartbeat
// traffic; internal hb_tick / connect_tick loops keep heartbeats flowing and
// retry dials for peers without a live connection. Identity adoption only
// happens on the dialer side (hello_ack sender == the proxy we dialed), so
// nodes never match peers by advertised address strings.
#include "shield/cluster/cluster_transport.hpp"

#include <algorithm>
#include <atomic>
#include <caf/actor_system.hpp>
#include <caf/error.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/io/middleman.hpp>
#include <caf/io/middleman_actor.hpp>
#include <caf/send.hpp>
#include <caf/stateful_actor.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shield/cluster/cluster_messages.hpp"
#include "shield/log/logger.hpp"

namespace shield::cluster {

namespace {

// Loop cadence floor so a misconfigured 0ms heartbeat cannot busy-loop.
constexpr int kMinTickMs = 50;

// Upper bound on a single dial. The middleman's blocking remote_actor()
// waits forever, which would freeze the connect loop when a peer restarts
// mid-dial, so we talk to the middleman actor directly with a timeout.
constexpr auto kDialTimeout = std::chrono::seconds(2);

std::chrono::milliseconds tick_interval(int heartbeat_interval_ms) {
    return std::chrono::milliseconds(
        std::max(heartbeat_interval_ms, kMinTickMs));
}

struct PeerLink {
    std::string address;  // dial target, "host:port"
    std::string host;
    uint16_t port = 0;
    std::string node_id;   // announced identity, learned at handshake
    caf::actor handle;     // invalid until the dial succeeds
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
    // Guards proxied_calls AND bridges: bridges are installed on a foreign
    // thread and cleared by stop() while the actor keeps dispatching.
    std::mutex data_mutex;
    // Proxied call sessions (M4): local session -> (remote call_session,
    // source node). Filled when an envelope call is dispatched, drained by
    // complete_proxied_call or callee-side expiry.
    std::unordered_map<uint64_t, std::pair<uint64_t, std::string>>
        proxied_calls;
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

// Send a reply to the node a call envelope came from. Drops silently (with
// a log) when the source connection is gone.
void reply_to(const SidePtr& side, const std::string& source_node,
              const EnvelopeReplyMsg& reply) {
    caf::actor handle;
    {
        std::shared_lock lock(side->peers_mutex);
        auto it = side->peers_by_node.find(source_node);
        if (it == side->peers_by_node.end()) {
            auto& log = shield::log::get_logger("cluster");
            SHIELD_LOG_WARNING(log, "No connection to " + source_node +
                                        "; dropping envelope reply");
            return;
        }
        handle = it->second;
    }
    side->tx_messages.fetch_add(1, std::memory_order_relaxed);
    caf::anon_send(handle, reply);
}

struct transport_state {
    ClusterManager* manager = nullptr;
    std::string self_node_id;
    uint64_t self_epoch = 0;
    int heartbeat_interval_ms = 5000;
    std::vector<PeerLink> peers;
    uint64_t hb_seq = 0;
};

using transport_actor = caf::stateful_actor<transport_state>;

caf::behavior transport_loop(transport_actor* self, ClusterManager* manager,
                             const ClusterConfig cfg, const SidePtr& side) {
    auto& st = self->state();
    st.manager = manager;
    st.self_node_id = cfg.node_id;
    st.self_epoch = manager->node_epoch();
    st.heartbeat_interval_ms = cfg.heartbeat_interval_ms;
    for (const auto& address : cfg.peers) {
        // Split "host:port". Invalid entries are skipped and logged; the
        // manager still tracks them so they degrade honestly.
        auto colon = address.rfind(':');
        if (colon == std::string::npos || colon == 0 ||
            colon + 1 == address.size()) {
            auto& log = shield::log::get_logger("cluster");
            SHIELD_LOG_ERROR(log, "Invalid peer address, skipping: " + address);
            continue;
        }
        PeerLink link;
        link.address = address;
        link.host = address.substr(0, colon);
        link.port = static_cast<uint16_t>(std::stoi(address.substr(colon + 1)));
        st.peers.push_back(std::move(link));
    }

    auto& log = shield::log::get_logger("cluster");

    self->set_down_handler([self, side](caf::down_msg& dm) {
        // The connection to a dialed peer died: definitive offline for that
        // node, routes invalidated; the connect loop redials.
        auto& st = self->state();
        for (auto& peer : st.peers) {
            if (peer.handle && peer.handle == dm.source) {
                st.manager->on_peer_down(peer.address);
                if (!peer.node_id.empty()) {
                    std::unique_lock lock(side->peers_mutex);
                    side->peers_by_node.erase(peer.node_id);
                }
                peer.dropped = true;
                peer.handle = nullptr;
                peer.node_id.clear();
                break;
            }
        }
    });

    return {
        // -- dial / retry loop ---------------------------------------------
        [self, side](connect_tick_atom) {
            auto& st = self->state();
            auto& log = shield::log::get_logger("cluster");
            for (auto& peer : st.peers) {
                if (peer.handle || peer.dialing) continue;
                peer.dialing = true;
                // Async dial through the middleman actor with a timeout; a
                // hung dial must never stall this loop, or a restarting peer
                // could never be picked up again.
                self->request(self->system().middleman().actor_handle(),
                              kDialTimeout, caf::connect_atom_v, peer.host,
                              peer.port)
                    .then(
                        [self, side, addr = peer.address](
                            caf::node_id, caf::strong_actor_ptr& ptr,
                            const std::set<std::string>&) {
                            auto& st = self->state();
                            auto& log = shield::log::get_logger("cluster");
                            for (auto& peer : st.peers) {
                                if (peer.address != addr) continue;
                                peer.dialing = false;
                                if (peer.handle || !ptr) return;
                                if (peer.dropped) {
                                    // A previous connection on this address
                                    // went down: this dial is a reconnect.
                                    peer.dropped = false;
                                    side->reconnects.fetch_add(
                                        1, std::memory_order_relaxed);
                                }
                                peer.handle = caf::actor_cast<caf::actor>(ptr);
                                self->monitor(peer.handle);
                                // Deliberately NOT anon_send: the hello must
                                // carry our address as sender, or the peer
                                // has nobody to ack to.
                                self->send(
                                    peer.handle,
                                    HelloMsg{st.self_node_id, st.self_epoch,
                                             kClusterProtoVersion});
                                SHIELD_LOG_INFO(log, "Dialed peer " + addr);
                                return;
                            }
                        },
                        [self, addr = peer.address](const caf::error& err) {
                            auto& st = self->state();
                            auto& log = shield::log::get_logger("cluster");
                            for (auto& peer : st.peers) {
                                if (peer.address == addr) {
                                    peer.dialing = false;
                                    break;
                                }
                            }
                            SHIELD_LOG_DEBUG(log, "Dial failed for " + addr +
                                                      ": " +
                                                      caf::to_string(err));
                        });
            }
            self->delayed_send(self, tick_interval(st.heartbeat_interval_ms),
                               connect_tick_atom_v);
        },
        // -- inbound handshake ---------------------------------------------
        [self](const HelloMsg& hello) {
            auto& log = shield::log::get_logger("cluster");
            SHIELD_LOG_DEBUG(log, "Hello from " + hello.node_id + " (epoch " +
                                      std::to_string(hello.epoch) + ")");
            if (hello.proto_version != kClusterProtoVersion) {
                SHIELD_LOG_ERROR(log, "Peer protocol version mismatch: got " +
                                          std::to_string(hello.proto_version) +
                                          ", expected " +
                                          std::to_string(kClusterProtoVersion));
                return;
            }
            // Reply over the inbound connection; the dialer adopts our
            // identity from this ack. We adopt nothing here — our own
            // adoption happens via our own dial (both sides dial). The ack
            // must carry our address as sender too: the dialer matches it
            // against the proxy it dialed.
            if (auto sender = self->current_sender()) {
                self->send(caf::actor_cast<caf::actor>(sender),
                           HelloAckMsg{self->state().self_node_id,
                                       self->state().self_epoch,
                                       kClusterProtoVersion});
            }
        },
        // -- handshake completion (dialer side) ------------------------------
        [self, side](const HelloAckMsg& ack) {
            auto& st = self->state();
            auto& log = shield::log::get_logger("cluster");
            SHIELD_LOG_DEBUG(log, "HelloAck from " + ack.node_id + " (epoch " +
                                      std::to_string(ack.epoch) + ")");
            if (ack.proto_version != kClusterProtoVersion) {
                SHIELD_LOG_ERROR(log,
                                 "Peer protocol version mismatch in ack: " +
                                     std::to_string(ack.proto_version));
                return;
            }
            // The ack's sender is exactly the proxy stored in one of the
            // dialed entries (reply over the same connection).
            bool matched = false;
            if (auto sender = self->current_sender()) {
                for (auto& peer : st.peers) {
                    if (peer.handle && peer.handle == sender) {
                        st.manager->on_handshake(peer.address, ack.node_id,
                                                 ack.epoch);
                        SHIELD_LOG_INFO(log, "Peer " + peer.address +
                                                 " identified as " +
                                                 ack.node_id);
                        // Adopt into the data-plane routing table (M4).
                        peer.node_id = ack.node_id;
                        {
                            std::unique_lock lock(side->peers_mutex);
                            side->peers_by_node[ack.node_id] = peer.handle;
                        }
                        matched = true;
                        // Publish our routes immediately so a joining peer
                        // converges without waiting for a heartbeat tick.
                        auto routes = st.manager->local_routes();
                        std::vector<RouteEntry> entries;
                        entries.reserve(routes.size());
                        for (auto& [name, service_id] : routes) {
                            entries.push_back(RouteEntry{name, service_id});
                        }
                        caf::anon_send(peer.handle,
                                       RoutesMsg{st.self_node_id, st.self_epoch,
                                                 std::move(entries)});
                        break;
                    }
                }
            }
            if (!matched) {
                SHIELD_LOG_WARNING(log, "HelloAck from " + ack.node_id +
                                            " does not match any dialed peer");
            }
        },
        // -- liveness --------------------------------------------------------
        [self, side](const HeartbeatMsg& hb) {
            side->rx_heartbeats.fetch_add(1, std::memory_order_relaxed);
            self->state().manager->on_heartbeat(hb.node_id);
        },
        [self, side](hb_tick_atom) {
            auto& st = self->state();
            // Full local route table snapshot rides along with every
            // heartbeat (M3): idempotent, self-healing, and an empty table
            // (service shutdown) propagates without a retract protocol.
            auto routes = st.manager->local_routes();
            std::vector<RouteEntry> entries;
            entries.reserve(routes.size());
            for (auto& [name, service_id] : routes) {
                entries.push_back(RouteEntry{name, service_id});
            }
            for (auto& peer : st.peers) {
                if (!peer.handle) continue;
                caf::anon_send(
                    peer.handle,
                    HeartbeatMsg{st.self_node_id, st.self_epoch, st.hb_seq++});
                side->tx_heartbeats.fetch_add(1, std::memory_order_relaxed);
                caf::anon_send(peer.handle, RoutesMsg{st.self_node_id,
                                                      st.self_epoch, entries});
            }
            self->delayed_send(self, tick_interval(st.heartbeat_interval_ms),
                               hb_tick_atom_v);
        },
        // -- inbound route table (M3) ----------------------------------------
        [self](const RoutesMsg& routes) {
            // The manager seam takes pairs to keep cluster_manager.hpp
            // decoupled from the wire header.
            std::vector<std::pair<std::string, std::string>> entries;
            entries.reserve(routes.routes.size());
            for (const auto& entry : routes.routes) {
                entries.emplace_back(entry.name, entry.service_id);
            }
            self->state().manager->on_routes(routes.node_id, routes.epoch,
                                             entries);
        },
        // -- inbound service envelope (M4) ------------------------------------
        [self, side](const EnvelopeMsg& env) {
            side->rx_messages.fetch_add(1, std::memory_order_relaxed);
            // Bridges are invoked OUTSIDE data_mutex (they re-enter the
            // transport from completion paths); snapshot each one under the
            // lock, then call.
            if (env.call_session == 0) {
                // Fire-and-forget: dispatch and log failures; there is no
                // reply channel for send.
                std::function<bool(const std::string&, const std::string&,
                                   const std::string&)>
                    send_dispatch;
                {
                    std::lock_guard lock(side->data_mutex);
                    send_dispatch = side->bridges.send_dispatch;
                }
                if (send_dispatch) {
                    if (!send_dispatch(env.service_id, env.method,
                                       env.args_json)) {
                        auto& log = shield::log::get_logger("cluster");
                        SHIELD_LOG_WARNING(
                            log, "Envelope send dispatch failed on " +
                                     env.service_id + "." + env.method);
                    }
                }
                return;
            }
            // Call: allocate the proxied session, register its routing entry
            // (remote session + source node) BEFORE dispatching — the callee
            // may complete on another thread before call_dispatch even
            // returns — then dispatch.
            std::function<uint64_t(int32_t)> call_begin;
            std::function<bool(uint64_t, const std::string&, const std::string&,
                               const std::string&, std::string*)>
                call_dispatch;
            {
                std::lock_guard lock(side->data_mutex);
                call_begin = side->bridges.call_begin;
                call_dispatch = side->bridges.call_dispatch;
            }
            uint64_t local = call_begin ? call_begin(env.timeout_ms) : 0;
            if (local == 0) {
                reply_to(side, env.source_node,
                         EnvelopeReplyMsg{
                             env.call_session, false, "", "service_not_found",
                             "service not found: " + env.service_id});
                return;
            }
            {
                std::lock_guard lock(side->data_mutex);
                side->proxied_calls[local] = {env.call_session,
                                              env.source_node};
            }
            std::string dispatch_error;
            const bool dispatched =
                call_dispatch &&
                call_dispatch(local, env.service_id, env.method, env.args_json,
                              &dispatch_error);
            if (!dispatched) {
                // Immediate dispatch failure (target gone): unregister and
                // fail the remote caller fast instead of letting it ride out
                // its timeout.
                {
                    std::lock_guard lock(side->data_mutex);
                    side->proxied_calls.erase(local);
                }
                reply_to(side, env.source_node,
                         EnvelopeReplyMsg{
                             env.call_session, false, "", "service_not_found",
                             dispatch_error.empty()
                                 ? "service not found: " + env.service_id
                                 : dispatch_error});
                return;
            }
        },
        // -- inbound envelope reply (M4) --------------------------------------
        [side](const EnvelopeReplyMsg& reply) {
            side->rx_messages.fetch_add(1, std::memory_order_relaxed);
            // Routes into the caller-side pending-call table, resuming the
            // suspended coroutine (unknown/expired sessions no-op there).
            // Invoked outside data_mutex: the completion hook re-enters
            // complete_proxied_call, which takes that mutex.
            std::function<void(uint64_t, bool, const std::string&,
                               const std::string&, const std::string&)>
                reply_handler;
            {
                std::lock_guard lock(side->data_mutex);
                reply_handler = side->bridges.reply_handler;
            }
            if (reply_handler) {
                reply_handler(reply.call_session, reply.ok, reply.payload_json,
                              reply.error_code, reply.error_message);
            }
        }};
}

}  // namespace

void init_cluster_caf_types() {
    caf::init_global_meta_objects<caf::id_block::shield_cluster>();
}

namespace {
ClusterTransport* g_cluster_transport = nullptr;
}

struct ClusterTransport::Impl {
    caf::actor_system* system = nullptr;
    ClusterManager* manager = nullptr;
    ClusterConfig config;
    caf::actor actor;
    uint16_t bound_port = 0;
    bool running = false;
    // Shared with the actor and with the facade methods below, which run on
    // whatever thread calls send_envelope / complete_proxied_call.
    SidePtr side = std::make_shared<TransportSideState>();
};

ClusterTransport::ClusterTransport(caf::actor_system& system,
                                   ClusterManager& manager,
                                   const ClusterConfig& config)
    : impl_(std::make_unique<Impl>()) {
    impl_->system = &system;
    impl_->manager = &manager;
    impl_->config = config;
}

ClusterTransport::~ClusterTransport() { stop(); }

bool ClusterTransport::start(uint16_t* bound_port, std::string& error) {
    if (impl_->running) {
        if (bound_port) *bound_port = impl_->bound_port;
        return true;
    }
    auto& log = shield::log::get_logger("cluster");

    // NOTE: init_cluster_caf_types() must have been called before the actor
    // system was constructed (CAF requirement), not here.

    impl_->actor = impl_->system->spawn(transport_loop, impl_->manager,
                                        impl_->config, impl_->side);

    // Publish on the configured listen address. "0.0.0.0" (or an empty host
    // part) publishes on all interfaces; port 0 lets the OS pick.
    auto colon = impl_->config.listen_address.rfind(':');
    std::string host = colon == std::string::npos
                           ? impl_->config.listen_address
                           : impl_->config.listen_address.substr(0, colon);
    uint16_t port = 0;
    if (colon != std::string::npos &&
        colon + 1 < impl_->config.listen_address.size()) {
        port = static_cast<uint16_t>(
            std::stoi(impl_->config.listen_address.substr(colon + 1)));
    }
    const char* bind_host = (host.empty() || host == "0.0.0.0" || host == "*")
                                ? nullptr
                                : host.c_str();
    auto published =
        impl_->system->middleman().publish(impl_->actor, port, bind_host);
    if (!published) {
        error = "cluster listen failed on " + impl_->config.listen_address +
                ": " + caf::to_string(published.error());
        SHIELD_LOG_ERROR(log, error);
        caf::anon_send_exit(impl_->actor, caf::exit_reason::user_shutdown);
        impl_->actor = nullptr;
        return false;
    }
    impl_->bound_port = *published;
    impl_->running = true;

    // Kick both loops.
    caf::anon_send(impl_->actor, connect_tick_atom_v);
    caf::anon_send(impl_->actor, hb_tick_atom_v);

    if (bound_port) *bound_port = impl_->bound_port;
    return true;
}

void ClusterTransport::stop() {
    if (!impl_->running) return;
    impl_->running = false;

    auto& log = shield::log::get_logger("cluster");
    SHIELD_LOG_INFO(log, "Cluster transport stopping");

    impl_->system->middleman().unpublish(impl_->actor);
    caf::anon_send_exit(impl_->actor, caf::exit_reason::user_shutdown);
    impl_->actor = nullptr;
    impl_->bound_port = 0;

    // Drop data-plane state so later complete_proxied_call / send_envelope
    // calls fail honestly instead of racing the dying actor. Bridges are
    // cleared too: a straggler envelope in the actor's mailbox must not
    // dispatch into a service manager that shutdown is already releasing.
    {
        std::unique_lock lock(impl_->side->peers_mutex);
        impl_->side->peers_by_node.clear();
    }
    {
        std::lock_guard lock(impl_->side->data_mutex);
        impl_->side->proxied_calls.clear();
        impl_->side->bridges = {};
    }
}

void ClusterTransport::set_envelope_bridges(EnvelopeBridges bridges) {
    std::lock_guard lock(impl_->side->data_mutex);
    impl_->side->bridges = std::move(bridges);
}

bool ClusterTransport::send_envelope(const std::string& target_node,
                                     const std::string& service_id,
                                     const std::string& method,
                                     const std::string& args_json,
                                     uint64_t call_session, int32_t timeout_ms,
                                     std::string* error) {
    caf::actor handle;
    {
        std::shared_lock lock(impl_->side->peers_mutex);
        auto it = impl_->side->peers_by_node.find(target_node);
        if (it == impl_->side->peers_by_node.end()) {
            if (error) *error = "node_offline";
            return false;
        }
        handle = it->second;
    }
    // Fire into the connection actor; BASP queues and delivers over TCP.
    // source_node lets the callee route the reply back without a reverse
    // lookup (both nodes dial each other, but source is authoritative).
    impl_->side->tx_messages.fetch_add(1, std::memory_order_relaxed);
    caf::anon_send(handle,
                   EnvelopeMsg{impl_->manager->node_id(), service_id, method,
                               args_json, call_session, timeout_ms});
    return true;
}

void ClusterTransport::complete_proxied_call(uint64_t local_session, bool ok,
                                             const std::string& payload_json,
                                             const std::string& error_code,
                                             const std::string& error_message) {
    uint64_t remote_session = 0;
    std::string source_node;
    {
        std::lock_guard lock(impl_->side->data_mutex);
        auto it = impl_->side->proxied_calls.find(local_session);
        if (it == impl_->side->proxied_calls.end()) return;  // already expired
        remote_session = it->second.first;
        source_node = it->second.second;
        impl_->side->proxied_calls.erase(it);
    }
    reply_to(impl_->side, source_node,
             EnvelopeReplyMsg{remote_session, ok, payload_json, error_code,
                              error_message});
}

ClusterTransport::Stats ClusterTransport::stats() const {
    Stats out;
    {
        std::shared_lock lock(impl_->side->peers_mutex);
        out.live_connections = impl_->side->peers_by_node.size();
    }
    out.reconnects = impl_->side->reconnects.load(std::memory_order_relaxed);
    out.tx_messages = impl_->side->tx_messages.load(std::memory_order_relaxed);
    out.rx_messages = impl_->side->rx_messages.load(std::memory_order_relaxed);
    out.tx_heartbeats =
        impl_->side->tx_heartbeats.load(std::memory_order_relaxed);
    out.rx_heartbeats =
        impl_->side->rx_heartbeats.load(std::memory_order_relaxed);
    return out;
}

ClusterTransport* global_cluster_transport() { return g_cluster_transport; }

void set_global_cluster_transport(ClusterTransport* transport) {
    g_cluster_transport = transport;
}

}  // namespace shield::cluster
