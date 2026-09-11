// [SHIELD_CONSOLE] Shared cluster snapshot builder (M5). See
// cluster_status.hpp.
#include "cluster_status.hpp"

#ifdef SHIELD_ENABLE_CLUSTER

#include <string>

#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"

namespace shield::console {

nlohmann::json build_cluster_status_json() {
    auto* cm = shield::cluster::global_cluster_manager();
    nlohmann::json cluster;
    cluster["node_id"] = cm->node_id();
    cluster["node_epoch"] = std::to_string(cm->node_epoch());
    if (auto* ct = shield::cluster::global_cluster_transport()) {
        const auto stats = ct->stats();
        cluster["connections"] = stats.live_connections;
        cluster["reconnects"] = stats.reconnects;
        cluster["tx_messages"] = stats.tx_messages;
        cluster["rx_messages"] = stats.rx_messages;
        cluster["tx_heartbeats"] = stats.tx_heartbeats;
        cluster["rx_heartbeats"] = stats.rx_heartbeats;
    }
    cluster["nodes"] = nlohmann::json::array();
    for (const auto& n : cm->nodes()) {
        // last_heartbeat_ms is a steady-clock value (no wall-clock meaning):
        // expose the derived age for consumers; null before the first beat.
        const int64_t age = cm->heartbeat_age_ms(n);
        nlohmann::json node = {
            {"node_id", n.node_id},
            {"address", n.address},
            {"state", shield::cluster::node_state_name(n.state)},
            {"epoch", std::to_string(n.epoch)},
            {"last_heartbeat_ms", n.last_heartbeat_ms},
            {"heartbeat_age_ms",
             age < 0 ? nlohmann::json() : nlohmann::json(age)}};
        cluster["nodes"].push_back(std::move(node));
    }
    return cluster;
}

}  // namespace shield::console

#endif  // SHIELD_ENABLE_CLUSTER
