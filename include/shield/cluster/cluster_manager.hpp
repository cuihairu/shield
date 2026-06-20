// [SHIELD_CLUSTER] Cluster manager for multi-node communication
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shield::cluster {

/// @brief Node state in the cluster
enum class NodeState {
    Connecting,   // TCP handshake in progress
    Online,       // Connected and heartbeating
    Suspect,      // Missed heartbeat, waiting for timeout
    Offline,      // Confirmed unreachable
    Removed,      // Removed from cluster
};

/// @brief Information about a cluster node
struct NodeInfo {
    std::string node_id;
    std::string address;       // host:port
    NodeState state = NodeState::Connecting;
    int64_t last_heartbeat_ms = 0;
    int64_t connected_at_ms = 0;
    uint64_t epoch = 0;        // node epoch for stale handle detection
};

/// @brief Cluster configuration parsed from YAML
struct ClusterConfig {
    bool enabled = false;
    std::string node_id;
    std::string listen_address;  // host:port
    std::vector<std::string> peers;  // list of host:port
    int heartbeat_interval_ms = 5000;
    int suspect_timeout_ms = 15000;
    int offline_timeout_ms = 30000;
};

/// @brief Callback for cross-node message delivery
using RemoteSendFn = std::function<bool(const std::string& target_node,
                                         const std::string& service_id,
                                         const std::string& method,
                                         const std::string& args_json)>;

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

    /// @brief Query a remote service name on a specific node.
    /// Returns empty string if node not found or name not resolved.
    std::string query_remote(const std::string& node_id,
                             const std::string& service_name) const;

    /// @brief Register a remote route: (node_id, service_name) -> service_id
    void register_route(const std::string& node_id,
                        const std::string& service_name,
                        const std::string& service_id);

    /// @brief Check if a target is on a remote node.
    /// Parses "node_id:service_name" format.
    /// @return true if target is remote, sets out_node and out_service
    static bool parse_remote_target(std::string_view target,
                                    std::string& out_node,
                                    std::string& out_service);

    /// @brief Set the function used to send messages to remote nodes
    void set_remote_send_fn(RemoteSendFn fn);

    /// @brief Send a message to a remote node
    bool send_remote(const std::string& target_node,
                     const std::string& service_id,
                     const std::string& method,
                     const std::string& args_json);

    /// @brief Check if a node is reachable (online or connecting)
    /// @return empty string if reachable, error code if not
    std::string check_node_reachable(const std::string& node_id) const;

    /// @brief Process cluster tick (heartbeat, timeout checks)
    /// @return Number of state changes
    int tick();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Parse cluster config from YAML
ClusterConfig parse_cluster_config();

}  // namespace shield::cluster
