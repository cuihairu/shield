// [SHIELD_CLUSTER] ClusterTransport — data-plane facade: envelope
// bridge installation, send_envelope / complete_proxied_call (M4)
// and the M5 stats snapshot, all running on foreign threads over
// the shared TransportSideState.
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
    caf::actor publisher;
    {
        std::shared_lock lock(impl_->side->peers_mutex);
        auto it = impl_->side->peers_by_node.find(target_node);
        if (it == impl_->side->peers_by_node.end()) {
            if (error) *error = "node_offline";
            return false;
        }
        handle = it->second;
        publisher = impl_->side->outbound_actor;
    }
    if (!publisher) {
        // GCOVR_EXCL_START (defensive: peers_by_node cannot outlive the
        // outbound actor — start() installs the actor before the loop can
        // adopt a peer, and stop() clears both under the same lock — so no
        // deterministic public-API drive can land here; only the stop()
        // clear-gap race between the two critical sections can)
        if (error) *error = "node_offline";
        return false;
        // GCOVR_EXCL_STOP
    }
    if (call_session != 0) {
        // Record the peer binding so an inbound reply is only routed to this
        // call when it comes from the node we sent it to. Reclaim entries for
        // calls that timed out without a reply (their caller-side session is
        // already gone, so any late reply is indistinguishable from a forgery).
        // call_session == 0 is fire-and-forget: no reply is expected, so no
        // binding is needed.
        std::lock_guard lock(impl_->side->data_mutex);
        const auto now = shield::base::now_ms();
        std::erase_if(impl_->side->outbound_sessions, [now](const auto& entry) {
            return entry.second.deadline_ms <= now;
        });
        // GCOVR_EXCL_START (compiler artifact: the effective-timeout ternary
        // gets its counts on the continuation line only — flow proves this
        // decl executes (the record below stores effective's value) — so the
        // decl line never reports a count)
        const int32_t effective =
            timeout_ms > 0 ? timeout_ms : kDefaultCallTimeoutMs;
        // GCOVR_EXCL_STOP
        impl_->side->outbound_sessions[call_session] = {
            target_node, now + static_cast<int64_t>(effective) + kReplyGraceMs};
    }
    // Queue on our transport actor so the wire message carries its actor
    // identity; the receiver binds that sender to the inbound Hello handshake.
    // GCOVR_EXCL_START (compiler artifact: the anon_send call and its
    // argument evaluations get their counts on the argument continuation
    // lines only — flow proves this statement line executes (return true is
    // reachable only through it) — so the statement line never reports a
    // count; the init-list branch artifacts of the EnvelopeMsg below are
    // subsumed by this region)
    caf::anon_send(publisher, dispatch_envelope_atom_v, LocalDispatch{}, handle,
                   EnvelopeMsg{impl_->manager->node_id(), service_id, method,
                               args_json, call_session, timeout_ms});
    // GCOVR_EXCL_STOP
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
             EnvelopeReplyMsg{
                 // GCOVR_EXCL_BR_LINE (compiler artifact:
                 // aggregate init-list branches)
                 remote_session, ok, payload_json, error_code, error_message});
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
}  // namespace shield::cluster
