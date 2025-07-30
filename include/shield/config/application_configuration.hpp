#pragma once

// Forward declaration to avoid circular dependency
namespace shield::core {
class ApplicationContext;
}

#include "shield/config/config.hpp"

namespace shield::config {

// Base Configuration class (equivalent to Spring Boot's @Configuration)
class Configuration {
public:
    virtual ~Configuration() = default;

    // Configure services and beans in the application context
    virtual void configure(shield::core::ApplicationContext& context) = 0;

    // Name of this configuration
    virtual std::string name() const = 0;

    // Optional: dependency configuration names (for ordering)
    virtual std::vector<std::string> depends_on() const { return {}; }
};

}  // namespace shield::config

// Include ApplicationContext after defining Configuration interface
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/core/application_context.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/gateway/gateway_service.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/metrics/prometheus_service.hpp"
#include "shield/script/lua_vm_pool.hpp"

namespace shield::config {

// Gateway Configuration (equivalent to Spring Boot's @Configuration)
class GatewayConfiguration : public Configuration {
public:
    void configure(shield::core::ApplicationContext& context) override {
        // Get configuration properties
        auto& config_manager = ConfigManager::instance();
        auto gateway_config =
            config_manager
                .get_configuration_properties<gateway::GatewayConfig>();

        if (!gateway_config) {
            throw std::runtime_error(
                "GatewayConfig not found. Make sure it's registered.");
        }

        // Get required dependencies
        auto actor_system =
            context.get_service<actor::DistributedActorSystem>();
        auto lua_vm_pool = context.get_service<script::LuaVMPool>();

        if (!actor_system || !lua_vm_pool) {
            throw std::runtime_error(
                "Required dependencies not found for GatewayService");
        }

        // Create GatewayService
        auto gateway_service = std::make_shared<gateway::GatewayService>(
            "gateway", *actor_system, *lua_vm_pool, gateway_config);

        // Register the service with ApplicationContext
        context.register_service("gateway", gateway_service);
    }

    std::string name() const override { return "GatewayConfiguration"; }

    std::vector<std::string> depends_on() const override {
        return {"ScriptConfiguration", "ActorConfiguration"};
    }
};

// Metrics Configuration
class MetricsConfiguration : public Configuration {
public:
    void configure(shield::core::ApplicationContext& context) override {
        // Get configuration properties
        auto& config_manager = ConfigManager::instance();
        auto prometheus_config =
            config_manager
                .get_configuration_properties<metrics::PrometheusConfig>();

        if (!prometheus_config) {
            // Use default config if not found
            prometheus_config = std::make_shared<metrics::PrometheusConfig>();
        }

        // Create PrometheusService (singleton pattern)
        auto prometheus_service = std::shared_ptr<metrics::PrometheusService>(
            &metrics::PrometheusService::instance(),
            [](metrics::PrometheusService*) {});

        // Register the service
        context.register_service("prometheus", prometheus_service);
    }

    std::string name() const override { return "MetricsConfiguration"; }
};

// Script Configuration
class ScriptConfiguration : public Configuration {
public:
    void configure(shield::core::ApplicationContext& context) override {
        // Create LuaVMPool with default configuration
        script::LuaVMPoolConfig lua_config;
        lua_config.initial_size = 4;
        lua_config.max_size = 16;
        lua_config.min_size = 2;

        auto lua_vm_pool =
            std::make_shared<script::LuaVMPool>("main_pool", lua_config);

        // Register the service
        context.register_service("lua_vm_pool", lua_vm_pool);
    }

    std::string name() const override { return "ScriptConfiguration"; }
};

// Main Application Configuration (aggregates all configurations)
class ApplicationConfiguration : public Configuration {
private:
    std::vector<std::unique_ptr<Configuration>> configurations_;

public:
    ApplicationConfiguration() {
        // Add all configuration classes in dependency order
        configurations_.push_back(std::make_unique<ScriptConfiguration>());
        configurations_.push_back(std::make_unique<MetricsConfiguration>());
        // Note: Commenting out GatewayConfiguration for now due to missing
        // dependencies
        // configurations_.push_back(std::make_unique<GatewayConfiguration>());
    }

    void configure(shield::core::ApplicationContext& context) override {
        // Configure all sub-configurations in order
        for (auto& config : configurations_) {
            try {
                config->configure(context);
                SHIELD_LOG_INFO << "Successfully configured: "
                                << config->name();
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "Failed to configure " << config->name()
                                 << ": " << e.what();
                throw;
            }
        }
    }

    std::string name() const override { return "ApplicationConfiguration"; }
};

}  // namespace shield::config