#pragma once

#include "shield/config/config.hpp"

namespace shield::config {

/**
 * Register all configuration properties with the ConfigManager.
 * This function should be called before loading any configuration files.
 */
void register_all_configuration_properties();

}  // namespace shield::config