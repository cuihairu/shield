// [SHIELD_CONSOLE] Shared global-capability snapshot for admin surfaces
// (P0). root.status, root.global and /ops/status render the same global
// JSON; one builder keeps the shapes from drifting.
#pragma once

#include <nlohmann/json.hpp>

#ifdef SHIELD_ENABLE_GLOBAL
namespace shield::console {

/// Builds the shield_global block shared by root.status, root.global and
/// /ops/status: data/cache counters, lock counts, rank totals, queue
/// counts and scheduler stats. Returns a null document when the manager
/// is not initialized (callers then omit the block).
nlohmann::json build_global_status_json();

}  // namespace shield::console
#endif
