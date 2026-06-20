// [SHIELD_CLUSTER] Cluster manager implementation
#include "shield/cluster/cluster_manager.hpp"

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <shared_mutex>

namespace shield::cluster {

struct ClusterManager::Impl {
    ClusterConfig config;
    uint64_t node_epoch = 0;
    std::unordered_map<std::string, NodeInfo> nodes;
    // Remote route cache: "node_id:service_name" -> service_id
    std::unordered_map<std::string, std::string> route_cache;
    mutable std::shared_mutex mutex;
    RemoteSendFn remote_send_fn;
    bool running = false;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string route_key(const std::string& node_id,
                          const std::string& service_name) const {
        return node_id + ":" + service_name;
    }

    void add_peer(const std::string& address) {
        // Extract node_id from address or use address as placeholder.
        // In Phase 1, peers are just addresses; node_id is learned during handshake.
        NodeInfo info;
        info.address = address;
        info.state = NodeState::Connecting;
        info.connected_at_ms = now_ms();
        // Use address as temporary node_id until handshake completes.
        info.node_id = address;
        nodes[address] = std::move(info);
    }
};

ClusterManager::ClusterManager(const ClusterConfig& config)
    : impl_(std::make_unique<Impl>(config)) {
    // Generate a random epoch for this node.
    std::random_device rd;
    std::mt19937_64 gen(rd());
    impl_->node_epoch = gen();

    // Add configured peers.
    for (const auto& peer : config.peers) {
        impl_->add_peer(peer);
    }
}

ClusterManager::~ClusterManager() {
    stop();
}

void ClusterManager::start() {
    if (impl_->running) return;
    impl_->running = true;

    auto& log = shield::log::get_logger("cluster");
    SHIELD_LOG_INFO(log, "Cluster starting: node_id=" + impl_->config.node_id +
                    " listen=" + impl_->config.listen_address +
                    " peers=" + std::to_string(impl_->config.peers.size()));

    // In Phase 1, peers start as "connecting" and move to "online" on first
    // successful heartbeat. Full CAF middleman integration is Phase 2+.
    // For now, mark all peers as online (static config assumption).
    for (auto& [id, node] : impl_->nodes) {
        node.state = NodeState::Online;
        node.last_heartbeat_ms = Impl::now_ms();
    }

    SHIELD_LOG_INFO(log, "Cluster started with " +
                    std::to_string(impl_->nodes.size()) + " peers");
}

void ClusterManager::stop() {
    if (!impl_->running) return;
    impl_->running = false;

    auto& log = shield::log::get_logger("cluster");
    SHIELD_LOG_INFO(log, "Cluster stopping");

    std::unique_lock lock(impl_->mutex);
    for (auto& [id, node] : impl_->nodes) {
        node.state = NodeState::Removed;
    }
}

const std::string& ClusterManager::node_id() const {
    return impl_->config.node_id;
}

uint64_t ClusterManager::node_epoch() const {
    return impl_->node_epoch;
}

std::vector<NodeInfo> ClusterManager::nodes() const {
    std::shared_lock lock(impl_->mutex);
    std::vector<NodeInfo> result;
    result.reserve(impl_->nodes.size());
    for (const auto& [id, node] : impl_->nodes) {
        result.push_back(node);
    }
    return result;
}

const NodeInfo* ClusterManager::find_node(const std::string& node_id) const {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->nodes.find(node_id);
    return it != impl_->nodes.end() ? &it->second : nullptr;
}

std::string ClusterManager::query_remote(const std::string& node_id,
                                          const std::string& service_name) const {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->route_cache.find(impl_->route_key(node_id, service_name));
    if (it != impl_->route_cache.end()) {
        return it->second;
    }
    // In Phase 1, return the service_name as-is (assumes remote uses same naming).
    return service_name;
}

void ClusterManager::register_route(const std::string& node_id,
                                     const std::string& service_name,
                                     const std::string& service_id) {
    std::unique_lock lock(impl_->mutex);
    impl_->route_cache[impl_->route_key(node_id, service_name)] = service_id;
}

bool ClusterManager::parse_remote_target(std::string_view target,
                                          std::string& out_node,
                                          std::string& out_service) {
    // Format: "node_id:service_name"
    auto colon = target.find(':');
    if (colon == std::string_view::npos || colon == 0 ||
        colon == target.size() - 1) {
        return false;
    }
    out_node = std::string(target.substr(0, colon));
    out_service = std::string(target.substr(colon + 1));
    return true;
}

void ClusterManager::set_remote_send_fn(RemoteSendFn fn) {
    std::unique_lock lock(impl_->mutex);
    impl_->remote_send_fn = std::move(fn);
}

bool ClusterManager::send_remote(const std::string& target_node,
                                  const std::string& service_id,
                                  const std::string& method,
                                  const std::string& args_json) {
    if (impl_->remote_send_fn) {
        return impl_->remote_send_fn(target_node, service_id, method, args_json);
    }
    return false;
}

std::string ClusterManager::check_node_reachable(const std::string& node_id) const {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->nodes.find(node_id);
    if (it == impl_->nodes.end()) {
        return "node_not_found";
    }
    switch (it->second.state) {
        case NodeState::Online:
        case NodeState::Connecting:
            return "";  // reachable
        case NodeState::Suspect:
            return "node_suspect";
        case NodeState::Offline:
            return "node_offline";
        case NodeState::Removed:
            return "node_removed";
    }
    return "node_offline";
}

int ClusterManager::tick() {
    if (!impl_->running) return 0;

    int changes = 0;
    const int64_t now = Impl::now_ms();

    std::unique_lock lock(impl_->mutex);
    for (auto& [id, node] : impl_->nodes) {
        if (node.state == NodeState::Online) {
            // Check if heartbeat timeout exceeded.
            if (now - node.last_heartbeat_ms > impl_->config.suspect_timeout_ms) {
                node.state = NodeState::Suspect;
                ++changes;
                auto& log = shield::log::get_logger("cluster");
                SHIELD_LOG_WARNING(log, "Node " + id + " is now suspect");
            }
        } else if (node.state == NodeState::Suspect) {
            if (now - node.last_heartbeat_ms > impl_->config.offline_timeout_ms) {
                node.state = NodeState::Offline;
                ++changes;
                auto& log = shield::log::get_logger("cluster");
                SHIELD_LOG_WARNING(log, "Node " + id + " is now offline");
            }
        }
    }
    return changes;
}

ClusterConfig parse_cluster_config() {
    auto& cfg = shield::config::global_config();
    ClusterConfig cc;

    cc.enabled = cfg.has("cluster.node_id");
    if (!cc.enabled) return cc;

    cc.node_id = cfg.get_string("cluster.node_id", "");
    cc.listen_address = cfg.get_string("cluster.listen", "0.0.0.0:9000");
    cc.heartbeat_interval_ms = static_cast<int>(
        cfg.get_int("cluster.heartbeat_interval_ms", 5000));
    cc.suspect_timeout_ms = static_cast<int>(
        cfg.get_int("cluster.suspect_timeout_ms", 15000));
    cc.offline_timeout_ms = static_cast<int>(
        cfg.get_int("cluster.offline_timeout_ms", 30000));

    // Parse peers list.
    if (cfg.has("cluster.peers")) {
        auto peers_str = cfg.get_string("cluster.peers", "");
        // Peers can be comma-separated or YAML list.
        // For simplicity, support comma-separated in Phase 1.
        std::string current;
        for (char c : peers_str) {
            if (c == ',' || c == '\n') {
                if (!current.empty()) {
                    // Trim whitespace
                    auto start = current.find_first_not_of(" \t");
                    auto end = current.find_last_not_of(" \t");
                    if (start != std::string::npos) {
                        cc.peers.push_back(current.substr(start, end - start + 1));
                    }
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            auto start = current.find_first_not_of(" \t");
            auto end = current.find_last_not_of(" \t");
            if (start != std::string::npos) {
                cc.peers.push_back(current.substr(start, end - start + 1));
            }
        }
    }

    return cc;
}

}  // namespace shield::cluster
