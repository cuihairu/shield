#include "shield/metrics/metrics_starter.hpp"

#include "shield/config/config.hpp"
#include "shield/core/application_context.hpp"
#include "shield/log/logger.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/metrics/prometheus_service.hpp"

namespace shield::metrics {

void MetricsStarter::initialize(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "Initializing Metrics Starter";

    // Get configuration properties
    auto& config_manager = config::ConfigManager::instance();
    auto prometheus_config =
        config_manager.get_configuration_properties<PrometheusConfig>();

    if (!prometheus_config) {
        // Use default config if not found
        prometheus_config = std::make_shared<PrometheusConfig>();
        SHIELD_LOG_WARN << "PrometheusConfig not found, using defaults";
    }

    // Create PrometheusService (singleton pattern)
    auto prometheus_service = std::shared_ptr<PrometheusService>(
        &PrometheusService::instance(), [](PrometheusService*) {
            // Custom deleter that does nothing (since it's a singleton)
        });

    // Register the service
    context.register_service("prometheus", prometheus_service);
    context.bind_config_reload<PrometheusConfig>(prometheus_service);

    SHIELD_LOG_INFO << "Metrics Starter initialized successfully";
}

}  // namespace shield::metrics
