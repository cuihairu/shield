// [SHIELD_CLUSTER] Cluster manager for multi-node communication
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace shield::cluster {

/// @brief Node state in the cluster
enum class NodeState {
    Connecting,  // TCP handshake in progress
    Online,      // Connected and heartbeating
    Suspect,     // Missed heartbeat, waiting for timeout
    Offline,     // Confirmed unreachable
    Removed,     // Removed from cluster
};

/// @brief Information about a cluster node
struct NodeInfo {
    std::string node_id;
    std::string address;  // host:port
    NodeState state = NodeState::Connecting;
    int64_t last_heartbeat_ms = 0;
    int64_t connected_at_ms = 0;
    uint64_t epoch = 0;  // node epoch for stale handle detection
};

/// @brief Cluster configuration parsed from YAML
struct ClusterConfig {
    bool enabled = false;
    std::string node_id;
    std::string listen_address;      // host:port
    std::vector<std::string> peers;  // list of host:port
    int heartbeat_interval_ms = 5000;
    int suspect_timeout_ms = 15000;
    int offline_timeout_ms = 30000;
};

/// @brief Callback for cross-node message delivery (M4). call_session != 0
/// marks a call envelope; timeout_ms carries the caller's remaining budget
/// (callee slack); on failure the transport sets *error (e.g. "node_offline").
using RemoteSendFn = std::function<bool(
    const std::string& target_node, const std::string& service_id,
    const std::string& method, const std::string& args_json,
    uint64_t call_session, int32_t timeout_ms, std::string* error)>;

/// @brief Cluster manager: manages node connections, heartbeat, and routing.
///
/// Phase 1 scope:
/// - Static peers from config
/// - Node handshake with node_id + epoch
/// - Heartbeat-driven lifecycle (online/suspect/offline)
/// - Remote route cache: (node_id, service_name) -> service_id
/// - shield.cluster.query(node, name) and shield.cluster.nodes()
class ClusterManager {
public:
    explicit ClusterManager(const ClusterConfig& config);
    ~ClusterManager();

    // Non-copyable
    ClusterManager(const ClusterManager&) = delete;
    ClusterManager& operator=(const ClusterManager&) = delete;

    /// @brief Start the cluster manager (connect to peers, start heartbeat)
    void start();

    /// @brief Stop the cluster manager
    void stop();

    /// @brief Get this node's ID
    const std::string& node_id() const;

    /// @brief Get this node's epoch
    uint64_t node_epoch() const;

    /// @brief Get all known nodes
    std::vector<NodeInfo> nodes() const;

    /// @brief Get info about a specific node
    /// @return NodeInfo pointer, or nullptr if not found
    const NodeInfo* find_node(const std::string& node_id) const;

    /// @brief M5 observability: milliseconds since this node's last
    /// heartbeat (or handshake) arrived. Returns -1 when none has arrived
    /// yet. Note last_heartbeat_ms itself is a steady-clock value, not a
    /// wall timestamp — read the age, never the raw field.
    int64_t heartbeat_age_ms(const NodeInfo& node) const;

    /// @brief Query a remote service name on a specific node.
    /// Returns empty string if node not found or name not resolved.
    std::string query_remote(const std::string& node_id,
                             const std::string& service_name) const;

    /// @brief Register a remote route: (node_id, service_name) -> service_id
    void register_route(const std::string& node_id,
                        const std::string& service_name,
                        const std::string& service_id);

    // -- Local route table (M3) ----------------------------------------------
    // The set of service names published by THIS node. Fed by the service
    // manager via on_local_route_changed; shipped to peers in full by the
    // transport at heartbeat cadence (RoutesMsg).

    /// @brief A local name was published (service_id non-empty) or retracted
    /// (service_id empty). Idempotent.
    void on_local_route_changed(const std::string& service_name,
                                const std::string& service_id);

    /// @brief Snapshot of the local route table, for the transport to ship.
    /// Vector of (name, service_id) pairs.
    std::vector<std::pair<std::string, std::string>> local_routes() const;

    /// @brief Check if a target is on a remote node.
    /// Parses "node_id:service_name" format.
    /// @return true if target is remote, sets out_node and out_service
    static bool parse_remote_target(std::string_view target,
                                    std::string& out_node,
                                    std::string& out_service);

    /// @brief Set the function used to send messages to remote nodes
    void set_remote_send_fn(RemoteSendFn fn);

    /// @brief Send a message (or call envelope) to a remote node
    bool send_remote(const std::string& target_node,
                     const std::string& service_id, const std::string& method,
                     const std::string& args_json, uint64_t call_session = 0,
                     int32_t timeout_ms = 0, std::string* error = nullptr);

    /// @brief Check if a node is reachable (online or connecting)
    /// @return empty string if reachable, error code if not
    std::string check_node_reachable(const std::string& node_id) const;

    // -- Transport seams (M2) ------------------------------------------------
    // ClusterTransport reports connection lifecycle events here; the manager
    // remains the single owner of node state.

    /// @brief The peer dialed at `address` completed the handshake and
    /// announced `node_id`/`epoch`. Adopts the identity: renames the
    /// placeholder entry (peers are keyed by address until then), marks the
    /// node Online and restarts the heartbeat clock. A changed epoch on a
    /// re-handshake (peer restarted) invalidates the node's cached routes.
    void on_handshake(const std::string& address, const std::string& node_id,
                      uint64_t epoch);

    /// @brief A heartbeat arrived from a known node: refresh liveness and
    /// restore Suspect/Offline nodes back to Online.
    void on_heartbeat(const std::string& node_id);

    /// @brief The connection to the peer dialed at `address` dropped: mark
    /// the node Offline (definitive, no suspect grace) and drop its cached
    /// routes.
    void on_peer_down(const std::string& address);

    /// @brief A full route table arrived from `node_id`. Requires the node to
    /// be adopted and the announced epoch to match; a stale instance's table
    /// is dropped. Replaces the node's whole route bucket (full-table sync).
    void on_routes(
        const std::string& node_id, uint64_t epoch,
        const std::vector<std::pair<std::string, std::string>>& routes);

    /// @brief Drop all cached routes learned for `node_id`.
    void clear_routes(const std::string& node_id);

    /// @brief Process cluster tick (heartbeat, timeout checks)
    /// @return Number of state changes
    int tick();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Parse cluster config from YAML
ClusterConfig parse_cluster_config();

/// @brief Runtime-owned cluster manager access for Lua bindings.
ClusterManager* global_cluster_manager();
void set_global_cluster_manager(ClusterManager* manager);

/// @brief Stable string name for Lua/admin snapshots.
std::string node_state_name(NodeState state);

}  // namespace shield::cluster
