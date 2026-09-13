// [SHIELD_CONSOLE] Shared server snapshot for admin surfaces (P0).
// root.server, root.status and /ops/status all render the same server JSON;
// one builder keeps the shapes from drifting.
#pragma once

#include <nlohmann/json.hpp>

#ifdef SHIELD_ENABLE_SERVER
namespace shield::console {

/// Builds the server block shared by root.server, root.status and
/// /ops/status: state machine state, uptime/version/node identity and the
/// watcher count. Requires ServerManager::global() != nullptr; the callers
/// check that.
nlohmann::json build_server_status_json();

}  // namespace shield::console
#endif
