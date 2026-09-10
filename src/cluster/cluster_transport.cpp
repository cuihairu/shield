// [SHIELD_CLUSTER] ClusterTransport implementation (M2).
//
// Shape: one published stateful actor handles inbound hello/heartbeat
// traffic; internal hb_tick / connect_tick loops keep heartbeats flowing and
// retry dials for peers without a live connection. Identity adoption only
// happens on the dialer side (hello_ack sender == the proxy we dialed), so
// nodes never match peers by advertised address strings.
#include "shield/cluster/cluster_transport.hpp"

#include <algorithm>
#include <caf/actor_system.hpp>
#include <caf/error.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/io/middleman.hpp>
#include <caf/io/middleman_actor.hpp>
#include <caf/send.hpp>
#include <caf/stateful_actor.hpp>
#include <chrono>
#include <string>
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
    caf::actor handle;     // invalid until the dial succeeds
    bool dialing = false;  // a dial request is in flight
};

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
                             const ClusterConfig cfg) {
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

    self->set_down_handler([self](caf::down_msg& dm) {
        // The connection to a dialed peer died: definitive offline for that
        // node, routes invalidated; the connect loop redials.
        auto& st = self->state();
        for (auto& peer : st.peers) {
            if (peer.handle && peer.handle == dm.source) {
                st.manager->on_peer_down(peer.address);
                peer.handle = nullptr;
                break;
            }
        }
    });

    return {
        // -- dial / retry loop ---------------------------------------------
        [self](connect_tick_atom) {
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
                        [self, addr = peer.address](
                            caf::node_id, caf::strong_actor_ptr& ptr,
                            const std::set<std::string>&) {
                            auto& st = self->state();
                            auto& log = shield::log::get_logger("cluster");
                            for (auto& peer : st.peers) {
                                if (peer.address != addr) continue;
                                peer.dialing = false;
                                if (peer.handle || !ptr) return;
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
        [self](const HelloAckMsg& ack) {
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
                        matched = true;
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
        [self](const HeartbeatMsg& hb) {
            self->state().manager->on_heartbeat(hb.node_id);
        },
        [self](hb_tick_atom) {
            auto& st = self->state();
            for (auto& peer : st.peers) {
                if (!peer.handle) continue;
                caf::anon_send(
                    peer.handle,
                    HeartbeatMsg{st.self_node_id, st.self_epoch, st.hb_seq++});
            }
            self->delayed_send(self, tick_interval(st.heartbeat_interval_ms),
                               hb_tick_atom_v);
        }};
}

}  // namespace

void init_cluster_caf_types() {
    caf::init_global_meta_objects<caf::id_block::shield_cluster>();
}

struct ClusterTransport::Impl {
    caf::actor_system* system = nullptr;
    ClusterManager* manager = nullptr;
    ClusterConfig config;
    caf::actor actor;
    uint16_t bound_port = 0;
    bool running = false;
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

    impl_->actor =
        impl_->system->spawn(transport_loop, impl_->manager, impl_->config);

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
}

}  // namespace shield::cluster
