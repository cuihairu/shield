// [SHIELD_CLUSTER] Pluggable node discovery for the cluster manager.
//
// Slice 1: backend seam + discovery loop + redis++ backend. The manager
// (cluster_manager.hpp) owns node state; discovery only feeds it peer
// addresses (add_peer) and reports disappearances.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shield/cluster/cluster_manager.hpp"

namespace shield::cluster {

/// @brief A node as published in the discovery backend.
struct DiscoveredNode {
    std::string node_id;
    std::string address;  // host:port, dial target for the transport
};

/// @brief Discovery backend seam: the storage calls NodeDiscovery makes.
/// Tests fake this with an in-memory map; production wires the redis++
/// implementation (make_redis_discovery_backend).
class NodeDiscoveryBackend {
public:
    virtual ~NodeDiscoveryBackend() = default;

    /// @brief Register/refresh this node's record (idempotent).
    virtual void register_node(const std::string& node_id,
                               const std::string& address, int ttl_seconds) = 0;
    /// @brief Heartbeat renewal (same record, fresh TTL + timestamp).
    virtual void heartbeat(const std::string& node_id, int ttl_seconds) = 0;
    /// @brief List every live node except this one (empty on first scan).
    virtual std::vector<DiscoveredNode> scan(const std::string& self_id) = 0;
    /// @brief Remove this node's record (graceful shutdown).
    virtual void unregister_node(const std::string& node_id) = 0;
};

/// @brief Discovery loop: registers this node, keeps the registration alive
/// with heartbeats, scans for peers, and reports appearances (dial) and
/// disappearances (suspect) via callbacks. Intervals are injectable so tests
/// can drive the loop in milliseconds.
class NodeDiscovery {
public:
    using DiscoveredFn = std::function<void(const DiscoveredNode&)>;
    using LostFn = std::function<void(const std::string& node_id)>;

    /// @param listen_address this node's dial address, advertised on register
    /// @param heartbeat_interval_ms registration renewal cadence
    /// @param scan_interval_ms peer list refresh cadence
    /// @param ttl_seconds registration TTL (backend-side expiry)
    NodeDiscovery(std::string node_id, std::string listen_address,
                  std::unique_ptr<NodeDiscoveryBackend> backend,
                  int heartbeat_interval_ms, int scan_interval_ms,
                  int ttl_seconds);
    ~NodeDiscovery();

    NodeDiscovery(const NodeDiscovery&) = delete;
    NodeDiscovery& operator=(const NodeDiscovery&) = delete;

    /// @brief Set the appearance/disappearance callbacks (before start()).
    void set_callbacks(DiscoveredFn on_discovered, LostFn on_lost);

    /// @brief Start the register/heartbeat/scan loop.
    void start();

    /// @brief Stop the loop and unregister this node.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Redis connection + key layout for discovery (design in
/// docs/runtime-cluster.md, "Redis 服务发现"). The node set is a Redis set
/// at `<prefix>`; each node's record is a hash at `<prefix>:<node_id>`
/// with addr/status/started_at/last_heartbeat fields. TTL renewal keeps
/// crashed nodes' records from outliving them; the scan treats records
/// older than the TTL as gone.
struct RedisDiscoveryConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    std::string prefix = "shield:nodes";
    int ttl_seconds = 10;
};

/// @brief Build the redis++-backed discovery backend. The connection is
/// lazy: the first backend call connects, so constructing it never blocks.
std::unique_ptr<NodeDiscoveryBackend> make_redis_discovery_backend(
    const RedisDiscoveryConfig& cfg);

/// @brief Build the discovery loop from the parsed `cluster.discovery`
/// config section (cluster_manager.hpp DiscoveryConfig). Returns nullptr
/// when the type is disabled ("") or unknown (a warning is logged).
std::unique_ptr<NodeDiscovery> make_node_discovery(
    const DiscoveryConfig& cfg, const std::string& node_id,
    const std::string& listen_address);

}  // namespace shield::cluster
