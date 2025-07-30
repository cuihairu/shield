#pragma once

#include "shield/core/starter.hpp"

namespace shield::metrics {

/**
 * Metrics Starter - Initializes metrics collection and Prometheus integration.
 * This Starter sets up the PrometheusService for metrics collection and export.
 */
class MetricsStarter : public core::IStarter {
public:
    void initialize(core::ApplicationContext& context) override;

    std::string name() const override { return "MetricsStarter"; }

    std::vector<std::string> depends_on() const override {
        // Metrics system has no dependencies
        return {};
    }
};

}  // namespace shield::metrics