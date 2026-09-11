// [SHIELD_CONSOLE] Shared cluster snapshot for admin surfaces (M5).
// root.status, root.cluster and /ops/status all render the same cluster
// JSON; one builder keeps the shapes from drifting.
#pragma once

#include <nlohmann/json.hpp>

#ifdef SHIELD_ENABLE_CLUSTER
namespace shield::console {

/// Builds the cluster block shared by root.status, root.cluster and
/// /ops/status: this node's identity, transport counters (live connections,
/// reconnects, message/heartbeat totals) and the per-peer snapshot with
/// heartbeat age. Reads the process-global manager/transport; counters are
/// only present while a transport is registered (a bare manager — e.g. a
/// unit fixture — reports the node snapshot alone). Requires
/// global_cluster_manager() != nullptr; the callers check that.
nlohmann::json build_cluster_status_json();

}  // namespace shield::console
#endif
