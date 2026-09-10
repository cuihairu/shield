// [SHIELD_CLUSTER] CAF middleman transport for peer handshake/heartbeat (M2).
//
// Scope: publishes the cluster actor on cluster.listen, dials every static
// peer, exchanges hello/hello_ack (identity adoption) and keeps heartbeats
// flowing so ClusterManager::tick() has real liveness to judge. Message
// payloads for the data plane (cross-node send/call) arrive in M4.
#pragma once

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::cluster
