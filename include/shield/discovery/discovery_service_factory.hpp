#pragma once

#include <memory>

#include "shield/discovery/discovery_config.hpp"
#include "shield/discovery/service_discovery.hpp"

namespace shield::discovery {

std::shared_ptr<IServiceDiscovery> create_discovery_service(
    const DiscoveryConfig* config);

}  // namespace shield::discovery
