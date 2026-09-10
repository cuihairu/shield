// [SHIELD_CLUSTER] Cluster manager implementation
#include "shield/cluster/cluster_manager.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <thread>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

namespace shield::cluster {

namespace {
ClusterManager* g_cluster_manager = nullptr;
}

struct ClusterManager::Impl {
    ClusterConfig config;
    uint64_t node_epoch = 0;
    std::unordered_map<std::string, NodeInfo> nodes;
    // Remote route cache, bucketed per node: node_id -> (name -> service_id).
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string>>
        route_cache;
    // Service names published by THIS node: name -> service_id. Shipped to
    // peers in full at heartbeat cadence (M3).
    std::unordered_map<std::string, std::string> local_routes;
    mutable std::shared_mutex mutex;
    RemoteSendFn remote_send_fn;
    bool running = false;
    // Drives run_tick() at heartbeat_interval_ms; started by start(),
    // joined by stop().
    std::jthread heartbeat_thread;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Degradation pass over node states. Called by the heartbeat thread and
    // by ClusterManager::tick() (tests / external schedulers).
    int run_tick() {
        if (!running) return 0;

        int changes = 0;
        const int64_t now = now_ms();

        std::unique_lock lock(mutex);
        for (auto& [id, node] : nodes) {
            if (node.state == NodeState::Connecting) {
                // Handshake never completed within the offline window: the
                // dial target is treated as confirmed unreachable.
                if (now - node.connected_at_ms > config.offline_timeout_ms) {
                    node.state = NodeState::Offline;
                    ++changes;
                    auto& log = shield::log::get_logger("cluster");
                    SHIELD_LOG_WARNING(log, "Node " + id +
                                                " handshake timed out, now "
                                                "offline");
                }
            } else if (node.state == NodeState::Online) {
                // Check if heartbeat timeout exceeded.
                if (now - node.last_heartbeat_ms > config.suspect_timeout_ms) {
                    node.state = NodeState::Suspect;
                    ++changes;
                    auto& log = shield::log::get_logger("cluster");
                    SHIELD_LOG_WARNING(log, "Node " + id + " is now suspect");
                }
            } else if (node.state == NodeState::Suspect) {
                if (now - node.last_heartbeat_ms > config.offline_timeout_ms) {
                    node.state = NodeState::Offline;
                    ++changes;
                    auto& log = shield::log::get_logger("cluster");
                    SHIELD_LOG_WARNING(log, "Node " + id + " is now offline");
                }
            }
        }
        return changes;
    }

    // Peers are keyed by node_id after adoption but by dial address before;
    // address is the only stable identifier across the rename.
    std::unordered_map<std::string, NodeInfo>::iterator find_by_address(
        const std::string& address) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            if (it->second.address == address) return it;
        }
        return nodes.end();
    }

    void add_peer(const std::string& address) {
        // Peers start in Connecting; the node_id is learned during the
        // handshake (on_handshake renames the entry).
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

ClusterManager::~ClusterManager() { stop(); }

void ClusterManager::start() {
    if (impl_->running) return;
    impl_->running = true;

    auto& log = shield::log::get_logger("cluster");
    SHIELD_LOG_INFO(log,
                    "Cluster starting: node_id=" + impl_->config.node_id +
                        " listen=" + impl_->config.listen_address +
                        " peers=" + std::to_string(impl_->config.peers.size()));

    // Phase 1 (M2): peers stay Connecting until the transport completes a
    // real handshake (on_handshake adopts the announced identity and marks
    // them Online). Without a transport they degrade honestly
    // (Connecting -> Offline) once the offline window lapses — run_tick()
    // below enforces that, so a configured-but-absent peer is never
    // reported as healthy.

    SHIELD_LOG_INFO(log, "Cluster started with " +
                             std::to_string(impl_->nodes.size()) + " peers");

    // Heartbeat scheduler: drives run_tick() at heartbeat_interval_ms so
    // node states degrade without an external caller. stop() halts it.
    impl_->heartbeat_thread =
        std::jthread([impl = impl_.get()](std::stop_token stop) {
            std::mutex wake_mutex;
            std::condition_variable_any wake_cv;
            std::unique_lock<std::mutex> wake_lock(wake_mutex);
            // Clamp the interval so a misconfigured 0ms cannot busy-loop.
            const auto interval = std::chrono::milliseconds(
                std::max(impl->config.heartbeat_interval_ms, 50));
            while (!stop.stop_requested()) {
                // run_tick() takes impl->mutex itself; the wake mutex here
                // is only for interruptible sleeping. The never-true
                // predicate makes the wait return on stop or timeout only.
                impl->run_tick();
                wake_cv.wait_for(wake_lock, stop, interval,
                                 [] { return false; });
            }
        });
}

void ClusterManager::stop() {
    if (!impl_->running) return;
    impl_->running = false;

    // Halt the scheduler before tearing node states down so tick() cannot
    // observe half-finished teardown.
    if (impl_->heartbeat_thread.joinable()) {
        impl_->heartbeat_thread.request_stop();
        impl_->heartbeat_thread.join();
    }

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

uint64_t ClusterManager::node_epoch() const { return impl_->node_epoch; }

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

std::string ClusterManager::query_remote(
    const std::string& node_id, const std::string& service_name) const {
    std::shared_lock lock(impl_->mutex);
    auto node_it = impl_->route_cache.find(node_id);
    if (node_it == impl_->route_cache.end()) return "";
    auto svc_it = node_it->second.find(service_name);
    return svc_it != node_it->second.end() ? svc_it->second : "";
}

void ClusterManager::register_route(const std::string& node_id,
                                    const std::string& service_name,
                                    const std::string& service_id) {
    std::unique_lock lock(impl_->mutex);
    impl_->route_cache[node_id][service_name] = service_id;
}

void ClusterManager::on_local_route_changed(const std::string& service_name,
                                            const std::string& service_id) {
    std::unique_lock lock(impl_->mutex);
    if (service_id.empty()) {
        impl_->local_routes.erase(service_name);
    } else {
        impl_->local_routes[service_name] = service_id;
    }
}

std::vector<std::pair<std::string, std::string>> ClusterManager::local_routes()
    const {
    std::shared_lock lock(impl_->mutex);
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(impl_->local_routes.size());
    for (const auto& [name, service_id] : impl_->local_routes) {
        result.emplace_back(name, service_id);
    }
    return result;
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
        return impl_->remote_send_fn(target_node, service_id, method,
                                     args_json);
    }
    return false;
}

void ClusterManager::on_handshake(const std::string& address,
                                  const std::string& node_id, uint64_t epoch) {
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->find_by_address(address);
    if (it == impl_->nodes.end()) return;  // unknown dial target
    auto& info = it->second;
    if (info.epoch != epoch) {
        // Fresh identity (first handshake, or the peer restarted and drew a
        // new epoch): cached routes for this node are stale.
        impl_->route_cache.erase(info.node_id);
        info.epoch = epoch;
    }
    info.state = NodeState::Online;
    info.last_heartbeat_ms = Impl::now_ms();
    if (info.node_id != node_id) {
        // Rename the placeholder entry under the announced identity.
        NodeInfo renamed = info;
        renamed.node_id = node_id;
        impl_->nodes.erase(it);
        impl_->nodes[node_id] = std::move(renamed);
    }
}

void ClusterManager::on_heartbeat(const std::string& node_id) {
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->nodes.find(node_id);
    if (it == impl_->nodes.end()) return;  // not adopted yet; hello will do
    auto& info = it->second;
    if (info.state == NodeState::Suspect || info.state == NodeState::Offline) {
        auto& log = shield::log::get_logger("cluster");
        SHIELD_LOG_INFO(log, "Node " + node_id + " is back online");
        info.state = NodeState::Online;
    }
    info.last_heartbeat_ms = Impl::now_ms();
}

void ClusterManager::on_peer_down(const std::string& address) {
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->find_by_address(address);
    if (it == impl_->nodes.end()) return;
    auto& log = shield::log::get_logger("cluster");
    SHIELD_LOG_WARNING(log, "Connection to peer " + it->second.node_id +
                                " lost, node is offline");
    it->second.state = NodeState::Offline;
    impl_->route_cache.erase(it->second.node_id);
}

void ClusterManager::on_routes(
    const std::string& node_id, uint64_t epoch,
    const std::vector<std::pair<std::string, std::string>>& routes) {
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->nodes.find(node_id);
    if (it == impl_->nodes.end()) return;  // not adopted yet
    if (it->second.epoch != epoch) {
        // Stale table from a dead instance: drop silently, the live one
        // re-announces on its next heartbeat.
        return;
    }
    auto& bucket = impl_->route_cache[node_id];
    bucket.clear();
    for (const auto& [name, service_id] : routes) {
        if (!service_id.empty()) bucket[name] = service_id;
    }
}

void ClusterManager::clear_routes(const std::string& node_id) {
    std::unique_lock lock(impl_->mutex);
    impl_->route_cache.erase(node_id);
}

std::string ClusterManager::check_node_reachable(
    const std::string& node_id) const {
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

int ClusterManager::tick() { return impl_->run_tick(); }

ClusterConfig parse_cluster_config() {
    auto& cfg = shield::config::global_config();
    ClusterConfig cc;

    cc.enabled = cfg.has("cluster.node_id");
    if (!cc.enabled) return cc;

    cc.node_id = cfg.get_string("cluster.node_id", "");
    cc.listen_address = cfg.get_string("cluster.listen", "0.0.0.0:9000");
    cc.heartbeat_interval_ms =
        static_cast<int>(cfg.get_int("cluster.heartbeat_interval_ms", 5000));
    cc.suspect_timeout_ms =
        static_cast<int>(cfg.get_int("cluster.suspect_timeout_ms", 15000));
    cc.offline_timeout_ms =
        static_cast<int>(cfg.get_int("cluster.offline_timeout_ms", 30000));

    // Parse peers list. Accept YAML sequences and comma/newline-separated
    // scalars.
    cc.peers = cfg.get_string_array("cluster.peers");
    if (cc.peers.empty() && cfg.has("cluster.peers")) {
        auto peers_str = cfg.get_string("cluster.peers", "");
        std::string current;
        for (char c : peers_str) {
            if (c == ',' || c == '\n') {
                if (!current.empty()) {
                    // Trim whitespace
                    auto start = current.find_first_not_of(" \t");
                    auto end = current.find_last_not_of(" \t");
                    if (start != std::string::npos) {
                        cc.peers.push_back(
                            current.substr(start, end - start + 1));
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

ClusterManager* global_cluster_manager() { return g_cluster_manager; }

void set_global_cluster_manager(ClusterManager* manager) {
    g_cluster_manager = manager;
}

std::string node_state_name(NodeState state) {
    switch (state) {
        case NodeState::Connecting:
            return "connecting";
        case NodeState::Online:
            return "online";
        case NodeState::Suspect:
            return "suspect";
        case NodeState::Offline:
            return "offline";
        case NodeState::Removed:
            return "removed";
    }
    return "unknown";
}

}  // namespace shield::cluster
