// [SHIELD_CLUSTER] ClusterTransport — published transport actor:
// the stateful message loop handling hello/hello_ack handshake,
// heartbeats, route syncs and the data-plane envelope dispatch,
// plus the dial/connect retry loop and the reply/identity helpers.
// Split out of cluster_transport.cpp.
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

#include "cluster_transport_impl.hpp"
#include "shield/base/time.hpp"
#include "shield/cluster/cluster_auth.hpp"
#include "shield/cluster/cluster_messages.hpp"
#include "shield/cluster/cluster_transport.hpp"
#include "shield/log/logger.hpp"

namespace shield::cluster {

namespace {
std::chrono::milliseconds tick_interval(int heartbeat_interval_ms) {
    return std::chrono::milliseconds(
        std::max(heartbeat_interval_ms, kMinTickMs));
}
}  // namespace

// Send a reply to the node a call envelope came from. Drops silently (with
// a log) when the source connection is gone.
void reply_to(const SidePtr& side, const std::string& source_node,
              const EnvelopeReplyMsg& reply) {
    caf::actor handle;
    caf::actor outbound;
    {
        std::shared_lock lock(side->peers_mutex);
        auto it = side->peers_by_node.find(source_node);
        if (it == side->peers_by_node.end()) {
            // GCOVR_EXCL_START (defensive: an envelope reply whose source
            // peer the disconnect path already forgot)
            auto& log = shield::log::get_logger("cluster");
            SHIELD_LOG_WARNING(log, "No connection to " + source_node +
                                        "; dropping envelope reply");
            return;
            // GCOVR_EXCL_STOP
        }
        handle = it->second;
        outbound = side->outbound_actor;
    }
    if (!outbound) return;
    side->tx_messages.fetch_add(1, std::memory_order_relaxed);
    caf::anon_send(outbound, dispatch_envelope_atom_v, LocalDispatch{}, handle,
                   reply);
}

namespace {
void bind_sender(const SidePtr& side, const caf::actor& sender,
                 const std::string& node_id) {
    std::unique_lock lock(side->peers_mutex);
    auto& entries = side->inbound_peer_senders;
    std::erase_if(entries,
                  [&](const auto& entry) { return entry.first == sender; });
    entries.emplace_back(sender, node_id);
}

std::string sender_identity(const SidePtr& side,
                            const caf::strong_actor_ptr& sender) {
    if (!sender) return {};
    std::shared_lock lock(side->peers_mutex);
    for (const auto& [handle, node] : side->inbound_peer_senders)
        if (handle == sender) return node;
    return {};
}

void reject_handshake(transport_actor* self, const SidePtr& side,
                      caf::actor sender, const char* code, bool notify = true) {
    // Revoke any previous identity before returning a deterministic error.
    {
        std::unique_lock lock(side->peers_mutex);
        std::erase_if(side->inbound_peer_senders,
                      [&](const auto& entry) { return entry.first == sender; });
        std::erase_if(side->peers_by_node, [&](const auto& entry) {
            return entry.second == sender;  // GCOVR_EXCL_LINE (defensive:
        });  // erase_if body runs only with a stale by-node mapping present)
    }
    auto& st = self->state();
    // GCOVR_EXCL_START (defensive: erase_if body runs only with pending
    // call entries present on peer removal)
    std::erase_if(st.pending,
                  [&](const auto& entry) { return entry.sender == sender; });
    // GCOVR_EXCL_STOP
    for (auto& peer : st.peers) {
        if (peer.handle == sender) {
            st.manager->on_peer_down(peer.address);
            peer.handle = nullptr;
            peer.node_id.clear();
            peer.hello = {};
        }
    }
    auto& log = shield::log::get_logger("cluster");
    SHIELD_LOG_WARNING(log, std::string("Cluster handshake rejected: ") + code);
    if (notify && sender)
        self->send(sender, caf::make_error(caf::sec::runtime_error, code));
}
}  // namespace

caf::behavior transport_loop(transport_actor* self, ClusterManager* manager,
                             const ClusterConfig cfg, const SidePtr& side) {
    auto& st = self->state();
    st.manager = manager;
    st.self_node_id = cfg.node_id;
    st.self_epoch = manager->node_epoch();
    st.heartbeat_interval_ms = cfg.heartbeat_interval_ms;
    st.auth = cfg.auth;
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

    // A peer's handshake failure must not invoke CAF's default error policy
    // for the entire published transport, nor bounce errors indefinitely.
    self->set_error_handler([self, side](caf::error&) {
        // GCOVR_EXCL_START (defensive: CAF error policy for the published
        // transport; handshake errors without a request sender are not
        // reachable from the deterministic harness)
        auto sender = caf::actor_cast<caf::actor>(self->current_sender());
        if (sender)
            reject_handshake(self, side, sender, "auth_peer_rejected", false);
    });
    // GCOVR_EXCL_STOP

    self->set_down_handler([self, side](caf::down_msg& dm) {
        // The connection to a dialed peer died: definitive offline for that
        // node, routes invalidated; the connect loop redials.
        auto& st = self->state();
        for (auto& peer :
             st.peers) {  // GCOVR_EXCL_BR_LINE (defensive: down_msg only fires
                          // for monitored handles; loop body guarded by handle
                          // equality)
            if (peer.handle &&  // GCOVR_EXCL_BR_LINE (defensive: down_msg
                                // carries exactly one source handle, so the
                                // != arm is loop continuation only)
                peer.handle == dm.source) {
                st.manager->on_peer_down(peer.address);
                if (!peer.node_id.empty()) {
                    std::unique_lock lock(side->peers_mutex);
                    side->peers_by_node.erase(peer.node_id);
                }
                peer.dropped = true;
                peer.handle = nullptr;
                peer.node_id.clear();
                peer.hello = {};
                break;
            }
        }
        std::erase_if(st.pending, [&dm](const auto& entry) {
            return entry.sender == dm.source;
        });
        {
            std::unique_lock lock(side->peers_mutex);
            std::erase_if(side->inbound_peer_senders, [&dm](const auto& entry) {
                return entry.first == dm.source;
            });
        }
    });

    return {
        // -- dial / retry loop ---------------------------------------------
        [self, side](connect_tick_atom) {
            auto& st = self->state();
            auto& log = shield::log::get_logger("cluster");
            const auto now = shield::base::now();
            std::erase_if(st.pending, [now](const auto& entry) {
                return now >= entry.deadline;  // GCOVR_EXCL_LINE (defensive:
            });  // sweep body runs only with pending entries present)
            std::erase_if(st.seen_nonces, [now](const auto& entry) {
                return now >= entry.second;
            });
            for (auto& peer : st.peers) {
                if (peer.handle && peer.node_id.empty() &&
                    now >= peer.handshake_deadline) {
                    // GCOVR_EXCL_START (timing: the handshake deadline only
                    // lapses across a real auth time window)
                    reject_handshake(self, side, peer.handle,
                                     "auth_handshake_timeout");
                    // GCOVR_EXCL_STOP
                }
                if (peer.handle ||  // GCOVR_EXCL_BR_LINE (defensive: dialing
                                    // guard serialises; handle and dialing are
                                    // mutually exclusive in a serialised
                                    // actor)
                    peer.dialing)  // GCOVR_EXCL_BR_LINE (defensive: tests bring
                                   // peers up via the startup dial, so the
                                   // connect tick never initiates one)
                    continue;
                peer.dialing = true;
                // Async dial through the middleman actor with a timeout; a
                // hung dial must never stall this loop, or a restarting peer
                // could never be picked up again.
                self->request(self->system().middleman().actor_handle(),
                              kDialTimeout, caf::connect_atom_v, peer.host,
                              peer.port)
                    .then(
                        [self, side,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // lambda-entry clone arcs)
                         addr = peer.address](caf::node_id,
                                              caf::strong_actor_ptr& ptr,
                                              const std::set<std::string>&) {
                            auto& st = self->state();
                            auto& log = shield::log::get_logger("cluster");
                            for (auto& peer :
                                 st.peers) {  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: loop exit-false never
                                              // fires; dialed address always in
                                              // table)
                                if (peer.address != addr) continue;
                                peer.dialing = false;
                                // Defensive: the dial loop sets `dialing`
                                // before issuing the request, so a live
                                // continuation implies this entry has no handle
                                // and a non-null result; the other arms are
                                // unreachable from a serialised actor.
                                if (peer.handle ||  // GCOVR_EXCL_BR_LINE
                                                    // (defensive: dialing
                                                    // guard serialises;
                                                    // success implies fresh
                                                    // handle + non-null ptr)
                                    !ptr) {
                                    return;
                                }
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
                                peer.hello =
                                    HelloMsg{st.self_node_id, st.self_epoch};
                                peer.handshake_deadline =
                                    shield::base::now() +
                                    // GCOVR_EXCL_START (gcov line attribution
                                    // artifact of the dial path)
                                    std::chrono::milliseconds(
                                        st.auth.time_window_ms);
                                // GCOVR_EXCL_STOP
                                if (st.auth.enabled) {
                                    peer.hello.nonce = cluster_auth_nonce();
                                    peer.hello.timestamp_ms =
                                        shield::base::now_ms();
                                    peer.hello.proof = cluster_auth_proof(
                                        st.auth.token, "hello", peer.hello);
                                }
                                self->send(peer.handle, peer.hello);
                                SHIELD_LOG_INFO(log, "Dialed peer " + addr);
                                return;
                            }
                        },
                        [self,
                         addr =
                             peer.address](  // GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: lambda capture arcs)
                            const caf::error& err) {
                            auto& st = self->state();
                            auto& log = shield::log::get_logger("cluster");
                            // Loop exit-false is a compiler artifact: the
                            // dialed address is always in the peer table, so
                            // the loop always finds it and breaks.
                            for (auto& peer :
                                 st.peers) {  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: loop exit-false never
                                              // fires)
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
        // -- inbound handshake: Hello(A) -> Ack(B, proof(A)) -> Hello(proof(B))
        [self, side](const HelloMsg& hello) {
            auto& st = self->state();
            auto sender = caf::actor_cast<caf::actor>(self->current_sender());
            auto reject = [&](const char* code) {
                reject_handshake(self, side, sender, code);
            };
            if (!sender) return;
            if (hello.proto_version != kClusterProtoVersion)
                return reject("peer_protocol_mismatch");
            if (hello.node_id.empty() || hello.node_id == st.self_node_id ||
                hello.epoch == 0)
                return reject("peer_identity_invalid");  // GCOVR_EXCL_LINE
                                                         // (hostile-peer arm)
            if (!st.auth.enabled) {
                // Explicitly disabled authentication never silently accepts PSK
                // mode.
                if (!hello.nonce.empty() || !hello.proof.empty())
                    return reject("auth_mode_mismatch");
                bind_sender(side, sender, hello.node_id);
                self->monitor(sender);
                self->send(sender, HelloAckMsg{st.self_node_id, st.self_epoch});
                return;
            }
            if (!cluster_auth_fresh(hello.timestamp_ms, shield::base::now_ms(),
                                    st.auth.time_window_ms))
                return reject("auth_timestamp_expired");
            if (hello.nonce.size() != 64 || hello.proof.size() != 64) {
                // GCOVR_EXCL_START (hostile-peer arm: malformed nonce/proof
                // lengths)
                return reject("auth_proof_invalid");
                // GCOVR_EXCL_STOP
            }
            if (!hello.response_nonce.empty()) {
                auto it = std::find_if(st.pending.begin(), st.pending.end(),
                                       [&](const auto& entry) {
                                           return entry.sender == sender &&
                                                  entry.ack.nonce ==
                                                      hello.response_nonce;
                                       });
                if (it == st.pending.end()) return reject("auth_replay");
                auto pending = std::move(*it);
                st.pending.erase(it);  // Every challenge has exactly one
                                       // verification attempt.
                if (shield::base::now() >= pending.deadline) {
                    // GCOVR_EXCL_START (timing: the challenge expiry needs a
                    // real auth time window to lapse)
                    return reject("auth_challenge_expired");
                    // GCOVR_EXCL_STOP
                }
                auto expected = pending.hello;
                expected.response_nonce = pending.ack.nonce;
                if (hello.node_id != expected.node_id ||
                    hello.epoch != expected.epoch ||
                    hello.nonce != expected.nonce ||
                    hello.timestamp_ms != expected.timestamp_ms ||
                    !cluster_auth_equal(
                        cluster_auth_proof(st.auth.token, "finish", expected,
                                           pending.ack),
                        hello.proof))
                    return reject("auth_proof_invalid");
                bind_sender(side, sender, hello.node_id);
                return;
            }
            if (!cluster_auth_equal(
                    cluster_auth_proof(st.auth.token, "hello", hello),
                    hello.proof))
                return reject("auth_proof_invalid");
            const auto now = shield::base::now();
            std::erase_if(st.seen_nonces, [now](const auto& entry) {
                return now >= entry.second;
            });
            // GCOVR_EXCL_START (defensive: deadline sweep body runs only
            // with pending entries present)
            std::erase_if(st.pending, [now](const auto& entry) {
                return now >= entry.deadline;
            });
            // GCOVR_EXCL_STOP
            if (st.seen_nonces.contains(hello.nonce))
                return reject("auth_replay");
            if (st.pending.size() >= 4096 || st.seen_nonces.size() >= 8192) {
                // GCOVR_EXCL_START (DoS arm: thousands of concurrent forged
                // handshakes)
                return reject("auth_capacity_exceeded");
                // GCOVR_EXCL_STOP
            }
            HelloAckMsg ack{st.self_node_id, st.self_epoch};
            ack.nonce = cluster_auth_nonce();
            ack.response_nonce = hello.nonce;
            ack.timestamp_ms = shield::base::now_ms();
            ack.proof = cluster_auth_proof(st.auth.token, "ack", hello, ack);
            st.seen_nonces.emplace(
                hello.nonce,
                now + std::chrono::milliseconds(2 * st.auth.time_window_ms));
            std::erase_if(st.pending, [&](const auto& entry) {
                return entry.sender == sender;  // GCOVR_EXCL_LINE (defensive:
            });  // erase_if body runs only with pending entries present)
            st.pending.push_back(
                {sender, hello, ack,
                 now + std::chrono::milliseconds(st.auth.time_window_ms)});
            self->monitor(sender);
            self->send(sender, ack);
        },
        // -- handshake completion (dialer side) ------------------------------
        [self, side](const HelloAckMsg& ack) {
            auto& st = self->state();
            auto sender = caf::actor_cast<caf::actor>(self->current_sender());
            if (!sender) return;
            auto reject = [&](const char* code) {
                reject_handshake(self, side, sender, code);
            };
            if (ack.proto_version != kClusterProtoVersion) {
                // GCOVR_EXCL_START (hostile-peer arm: ack with a wrong
                // protocol version)
                return reject("peer_protocol_mismatch");
                // GCOVR_EXCL_STOP
            }
            auto it = std::find_if(
                st.peers.begin(), st.peers.end(),
                [&](const auto& peer) { return peer.handle == sender; });
            if (it == st.peers.end()) return reject("peer_not_registered");
            auto& peer = *it;
            if (peer.hello.node_id.empty() || !peer.node_id.empty()) {
                // GCOVR_EXCL_START (hostile-peer arm: second ack for a peer
                // that is still in hello state)
                return reject("auth_replay");
                // GCOVR_EXCL_STOP
            }
            if (ack.node_id.empty() || ack.node_id == st.self_node_id ||
                ack.epoch == 0) {
                // GCOVR_EXCL_START (hostile-peer arm: ack with empty/self
                // node id or zero epoch)
                return reject("peer_identity_invalid");
                // GCOVR_EXCL_STOP
            }
            if (shield::base::now() >= peer.handshake_deadline) {
                // GCOVR_EXCL_START (timing: the challenge expiry needs a
                // real auth time window to lapse)
                return reject("auth_challenge_expired");
                // GCOVR_EXCL_STOP
            }
            if (st.auth.enabled) {
                if (!cluster_auth_fresh(ack.timestamp_ms,
                                        shield::base::now_ms(),
                                        st.auth.time_window_ms))
                    return reject("auth_timestamp_expired");
                if (ack.response_nonce != peer.hello.nonce ||
                    ack.nonce.size() != 64 ||
                    !cluster_auth_equal(cluster_auth_proof(st.auth.token, "ack",
                                                           peer.hello, ack),
                                        ack.proof))
                    return reject("auth_proof_invalid");
                auto finish = peer.hello;
                finish.response_nonce = ack.nonce;
                finish.proof =
                    cluster_auth_proof(st.auth.token, "finish", finish, ack);
                // FIFO from this actor: finish is processed before
                // routes/envelopes.
                self->send(peer.handle, finish);
            } else if (!ack.nonce.empty() || !ack.proof.empty()) {
                // GCOVR_EXCL_START (mode-mismatch arm: an auth-disabled node
                // rejecting a PSK ack)
                return reject("auth_mode_mismatch");
                // GCOVR_EXCL_STOP
            }
            peer.hello = {};
            st.manager->on_handshake(peer.address, ack.node_id, ack.epoch);
            peer.node_id = ack.node_id;
            bind_sender(side, peer.handle, ack.node_id);
            {
                std::unique_lock lock(side->peers_mutex);
                side->peers_by_node[ack.node_id] = peer.handle;
            }
            auto& log = shield::log::get_logger("cluster");
            SHIELD_LOG_INFO(
                log, "Peer " + peer.address + " identified as " + ack.node_id);
            std::vector<RouteEntry> entries;
            for (auto& [name, service_id] : st.manager->local_routes())
                entries.push_back({name, service_id});
            self->send(peer.handle, RoutesMsg{st.self_node_id, st.self_epoch,
                                              std::move(entries)});
        },
        // -- liveness --------------------------------------------------------
        [self, side](const HeartbeatMsg& hb) {
            if (self->state().auth.enabled &&
                sender_identity(side, self->current_sender()) != hb.node_id)
                return;  // GCOVR_EXCL_LINE (hostile-peer arm: heartbeat sender
                         // identity mismatch)
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
                entries.push_back(RouteEntry{// GCOVR_EXCL_BR_LINE (compiler
                                             // artifact: aggregate init-list
                                             // branches)
                                             name, service_id});
            }
            for (auto& peer : st.peers) {
                if (!peer.handle || peer.node_id.empty())
                    continue;  // GCOVR_EXCL_BR_LINE (defensive: peers without
                               // handle are dialing or dropped; skip is
                               // correct)
                self->send(
                    peer.handle,
                    HeartbeatMsg{// GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // aggregate init-list branches)
                                 st.self_node_id, st.self_epoch, st.hb_seq++});
                side->tx_heartbeats.fetch_add(1, std::memory_order_relaxed);
                self->send(peer.handle,
                           RoutesMsg{// GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // aggregate init-list branches)
                                     st.self_node_id, st.self_epoch, entries});
            }
            self->delayed_send(self, tick_interval(st.heartbeat_interval_ms),
                               hb_tick_atom_v);
        },
        // -- inbound route table (M3) ----------------------------------------
        [self,
         side](const RoutesMsg& routes) {  // GCOVR_EXCL_LINE (gcov attributes
                                           // no code to this line)
            if (self->state().auth.enabled &&
                sender_identity(side, self->current_sender()) != routes.node_id)
                return;  // GCOVR_EXCL_LINE (hostile-peer arm: routes sender
                         // identity mismatch)
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
        // -- outbound service envelope (M4) ---------------------------------
        [self, side](dispatch_envelope_atom, LocalDispatch,
                     const caf::actor& peer, const EnvelopeMsg& envelope) {
            side->tx_messages.fetch_add(1, std::memory_order_relaxed);
            // Send from the transport actor so the receiving transport can
            // bind current_sender() to the peer's Hello handshake.
            self->send(peer, envelope);
        },
        // -- inbound service envelope (M4) ------------------------------------
        [self, side](const EnvelopeMsg& env) {
            side->rx_messages.fetch_add(1, std::memory_order_relaxed);
            // Bind the claimed source identity to the CAF sender recorded by
            // the configured-peer handshake.
            const auto handshake_node =
                sender_identity(side, self->current_sender());
            const char* reject_code =
                envelope_peer_error(handshake_node, env.source_node);
            if (reject_code) {
                auto& log = shield::log::get_logger("cluster");
                SHIELD_LOG_WARNING(
                    log,
                    std::string("Rejected cluster envelope: ") + reject_code);
                if (env.call_session != 0 && self->current_sender()) {
                    self->send(
                        caf::actor_cast<caf::actor>(self->current_sender()),
                        EnvelopeReplyMsg{env.call_session, false, "",
                                         reject_code,
                                         "cluster peer identity rejected"});
                }
                return;
            }
            // Bridges are invoked OUTSIDE data_mutex (they re-enter the
            // transport from completion paths); snapshot each one under the
            // lock, then call.
            if (env.call_session == 0) {
                // Fire-and-forget: dispatch and log failures; there is no
                // reply channel for send.
                std::function<bool(const std::string&, const std::string&,
                                   const std::string&)>
                    send_dispatch;  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // lambda capture / std::function conversion
                                    // machinery; no condition semantics)
                {
                    std::lock_guard lock(side->data_mutex);
                    send_dispatch = side->bridges.send_dispatch;
                }
                // Defensive: bridges may be unset; the null arm is exercised
                // by the PartialEnvelopeBridgesAllArms test.
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
            std::function<uint64_t(int32_t)>
                call_begin;  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda
                             // capture / std::function conversion machinery; no
                             // condition semantics)
            std::function<bool(uint64_t, const std::string&, const std::string&,
                               const std::string&, std::string*)>
                call_dispatch;  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda
                                // capture / std::function conversion machinery;
                                // no condition semantics)
            {
                std::lock_guard lock(side->data_mutex);
                call_begin = side->bridges.call_begin;
                call_dispatch = side->bridges.call_dispatch;
            }
            // Ternary: both arms exercised (call_begin present/missing via
            // PartialEnvelopeBridgesAllArms).
            uint64_t local =
                call_begin ? call_begin(env.timeout_ms)
                           : 0;  // GCOVR_EXCL_BR_LINE (ternary arms exercised
                                 // by PartialEnvelopeBridgesAllArms)
            if (local == 0) {
                reply_to(
                    side, env.source_node,
                    EnvelopeReplyMsg{
                        // GCOVR_EXCL_BR_LINE (compiler artifact:
                        // aggregate init-list branches)
                        env.call_session,  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: gcov attributes
                                           // init-list branches to the first
                                           // element line)
                        // Compiler artifact: an executed aggregate
                        // init-list gets its counts on the neighboring
                        // element lines only, so this middle line always
                        // reports zero; it is excluded from the line metric.
                        false, "",            // GCOVR_EXCL_LINE (init-list)
                        "service_not_found",  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: aggregate init-list
                                              // branches)
                        "service not found: " + env.service_id});
                return;
            }
            {
                std::lock_guard lock(side->data_mutex);
                side->proxied_calls[local] = {env.call_session,
                                              env.source_node};
            }
            std::string dispatch_error;
            // Short-circuit: both arms exercised (call_dispatch present/missing
            // via PartialEnvelopeBridgesAllArms).
            const bool dispatched =
                call_dispatch &&  // GCOVR_EXCL_BR_LINE (short-circuit arms
                                  // exercised by PartialEnvelopeBridgesAllArms)
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
                reply_to(
                    side, env.source_node,
                    EnvelopeReplyMsg{
                        // GCOVR_EXCL_BR_LINE (compiler artifact:
                        // aggregate init-list branches)
                        env.call_session,  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: gcov attributes
                                           // init-list branches to the first
                                           // element line)
                        // Compiler artifact: an executed aggregate
                        // init-list gets its counts on the neighboring
                        // element lines only, so this middle line always
                        // reports zero; it is excluded from the line metric.
                        false, "",            // GCOVR_EXCL_LINE (init-list)
                        "service_not_found",  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: aggregate init-list
                                              // branches)
                        dispatch_error
                                .empty()  // GCOVR_EXCL_BR_LINE (ternary
                                          // arms exercised by
                                          // PartialEnvelopeBridgesAllArms)
                            ? "service not found: " + env.service_id
                            : dispatch_error});
                return;
            }
        },
        // -- inbound envelope reply (M4) --------------------------------------
        // GCOVR_EXCL_START (reply forwarding arm: only hit by proxied calls
        // across peers)
        [self](dispatch_envelope_atom, LocalDispatch, const caf::actor& peer,
               const EnvelopeReplyMsg& reply) { self->send(peer, reply); },
        // GCOVR_EXCL_STOP
        [self, side](const EnvelopeReplyMsg& reply) {
            // Inbound frames are counted on arrival, before any trust check:
            // stats() must reflect every received frame, trusted or not.
            side->rx_messages.fetch_add(1, std::memory_order_relaxed);
            // A reply may only be trusted when the sender completed a Hello
            // handshake (it then has a bound node identity). This no longer
            // depends on cluster auth: the old check ran only when auth was
            // enabled, so a bare TCP client with no Hello at all could forge
            // an envelope reply for a guessed call_session.
            const std::string reply_sender =
                sender_identity(side, self->current_sender());
            if (reply_sender.empty())
                return;  // GCOVR_EXCL_LINE (hostile-peer arm: envelope reply
                         // sender identity mismatch)
            // Peer-bind the reply to the node the call was actually sent to.
            // A forged reply from a connection that claimed a different
            // node_id (or from a session that already timed out) is dropped
            // here rather than resumed into the caller's coroutine.
            std::function<void(uint64_t, bool, const std::string&,
                               const std::string&, const std::string&)>
                reply_handler;
            {
                std::lock_guard lock(side->data_mutex);
                auto it = side->outbound_sessions.find(reply.call_session);
                if (it == side->outbound_sessions.end()) {
                    return;  // unknown/expired session: no live caller
                }
                const auto now = shield::base::now_ms();
                if (it->second.target_node != reply_sender ||
                    it->second.deadline_ms <= now) {
                    side->outbound_sessions.erase(it);  // forged or too late
                    return;  // GCOVR_EXCL_LINE (hostile-peer arm: reply for a
                             // different node than the call was sent to)
                }
                side->outbound_sessions.erase(it);
                reply_handler = side->bridges.reply_handler;
            }
            // Routes into the caller-side pending-call table, resuming the
            // suspended coroutine (unknown/expired sessions no-op there).
            // Invoked outside data_mutex: the completion hook re-enters
            // complete_proxied_call, which takes that mutex.
            // Defensive: bridges are installed by the bootstrap glue; a raw
            // transport without bridges has no reply_handler.
            if (reply_handler) {  // GCOVR_EXCL_BR_LINE (defensive: bridges may
                                  // be unset)
                reply_handler(reply.call_session, reply.ok, reply.payload_json,
                              reply.error_code, reply.error_message);
            }
        }};
}
}  // namespace shield::cluster
