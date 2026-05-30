// [CORE]
#pragma once

#include <string>

#include "shield/protocol/protocol_handler.hpp"

namespace shield::actor {
class DistributedActorSystem;
}

namespace shield::gateway {

// Builds runtime diagnostic responses for HTTP endpoints.
// Used by GatewayService to serve /health/detailed, /status/runtime, etc.
class RuntimeDiagnostics {
public:
    // Basic health: {"status":"ok"}
    static std::string health_json();

    // Detailed health: integrates HealthCheckRegistry if available
    static std::string health_detailed_json();

    // Runtime status: services, actors count, node info
    static std::string runtime_status_json(
        actor::DistributedActorSystem& dist_system);

    // Configuration reload rules documentation
    static std::string config_reload_info_json();
};

}  // namespace shield::gateway
