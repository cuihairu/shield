// [SHIELD_CLUSTER] CAF middleman transport for peer handshake/heartbeat (M2).
//
// Scope: publishes the cluster actor on cluster.listen, dials every static
// peer, exchanges hello/hello_ack (identity adoption) and keeps heartbeats
// flowing so ClusterManager::tick() has real liveness to judge. Message
// payloads for the data plane (cross-node send/call) arrive in M4.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "shield/cluster/cluster_manager.hpp"

namespace caf {
class actor_system;
}

namespace shield::cluster {

/// Registers the cluster message types with CAF's global meta object table.
/// MUST run before any caf::actor_system is constructed. Declared in
/// cluster_messages.hpp; re-declared here for convenience.
void init_cluster_caf_types();

/// Bridges between inbound envelopes and this node's service runtime (M4).
/// All callbacks are invoked on the transport actor's dispatch thread, but
/// always OUTSIDE the transport's data mutex: they dispatch into the service
/// manager, whose completions re-enter the transport (complete_proxied_call
/// takes that mutex). Implementations must be thread-safe against the
/// service manager, which all of them are.
struct EnvelopeBridges {
    /// Fire-and-forget envelope (call_session == 0): dispatch against the
    /// local service manager. Returning false logs; there is no reply
    /// channel for send.
    std::function<bool(const std::string& service_id, const std::string& method,
                       const std::string& args_json)>
        send_dispatch;

    /// Call envelope, phase 1: allocate a proxied session (pending entry +
    /// expiry deadline) without dispatching anything. Returns the local
    /// session id, or 0 when the allocation was refused. The transport
    /// records the routing entry (remote session + source node) BEFORE phase
    /// 2 so a fast callee completion can never race it.
    std::function<uint64_t(int32_t timeout_ms)> call_begin;

    /// Call envelope, phase 2: dispatch a session allocated by call_begin
    /// (send the call request to the target service and arm its expiry
    /// driver). Returning false fails the call immediately; the transport
    /// then unregisters the routing entry and replies service_not_found to
    /// the caller's node.
    std::function<bool(uint64_t session, const std::string& service_id,
                       const std::string& method, const std::string& args_json,
                       std::string* error)>
        call_dispatch;

    /// Inbound envelope reply: complete the caller-side session (routes
    /// into the pending-call table, resuming the suspended coroutine).
    std::function<void(
        uint64_t call_session, bool ok, const std::string& payload_json,
        const std::string& error_code, const std::string& error_message)>
        reply_handler;
};

/// CAF-middleman-based peer transport. Owns the published cluster actor and
/// one outbound connection per configured peer, with reconnect retries at
/// heartbeat cadence. Reports lifecycle events into ClusterManager via the
/// on_handshake / on_heartbeat / on_peer_down seams.
class ClusterTransport {
public:
    /// @param system CAF actor system with the io middleman loaded.
    /// @param manager Node state owner; must outlive the transport.
    /// @param config Cluster config (node identity, listen address, peers,
    ///               heartbeat cadence).
    ClusterTransport(caf::actor_system& system, ClusterManager& manager,
                     const ClusterConfig& config);
    ~ClusterTransport();

    // Non-copyable
    ClusterTransport(const ClusterTransport&) = delete;
    ClusterTransport& operator=(const ClusterTransport&) = delete;

    /// @brief Publish the cluster actor on config.listen_address and start
    /// dialing all peers. Idempotent after a successful start.
    /// @param error Set on failure (publish/bind problems).
    /// @return The actual bound port.
    bool start(uint16_t* bound_port, std::string& error);

    /// @brief Unpublish and tear down the actor; stops dialing. Idempotent.
    void stop();

    // -- M4 data plane -------------------------------------------------------

    /// @brief Install the data-plane bridges. Call once at bootstrap before
    /// any envelope traffic; replacing or clearing at runtime is not
    /// supported.
    void set_envelope_bridges(EnvelopeBridges bridges);

    /// @brief Queue an envelope to target_node over its live connection.
    /// Thread-safe. Fails fast when the node is unknown or has no live
    /// connection (error = "node_offline").
    bool send_envelope(const std::string& target_node,
                       const std::string& service_id, const std::string& method,
                       const std::string& args_json, uint64_t call_session,
                       int32_t timeout_ms, std::string* error);

    /// @brief Completion ingress for a proxied (remotely originated) call
    /// session: looks up the envelope it came from and replies to the
    /// source node. Thread-safe; unknown sessions are dropped silently (the
    /// entry may have expired on the callee side).
    void complete_proxied_call(uint64_t local_session, bool ok,
                               const std::string& payload_json,
                               const std::string& error_code,
                               const std::string& error_message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::cluster
