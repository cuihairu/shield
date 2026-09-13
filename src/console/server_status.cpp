// [SHIELD_CONSOLE] Shared server snapshot builder (P0). See
// server_status.hpp.
#include "server_status.hpp"

#ifdef SHIELD_ENABLE_SERVER

#include "shield/server/server_manager.hpp"

namespace shield::console {

nlohmann::json build_server_status_json() {
    auto* sm = shield::server::ServerManager::global();
    nlohmann::json server;
    server["state"] = shield::server::server_state_name(sm->state());
    server["uptime_seconds"] = sm->uptime_seconds();
    server["version"] = sm->version();
    server["node_id"] = sm->node_id();
    server["started_at_ms"] = sm->started_at_ms();
    server["name"] = sm->config().name;
    server["info"] = {{"name", sm->config().info_name},
                      {"version", sm->config().info_version},
                      {"region", sm->config().info_region}};
    server["watchers"] = sm->watcher_count();
    server["shutdown_scheduled"] = sm->shutdown_scheduled();
    return server;
}

}  // namespace shield::console

#endif  // SHIELD_ENABLE_SERVER
