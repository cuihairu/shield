// [CORE]
#pragma once

// Forward declaration to avoid circular dependency
namespace shield::core {
class ApplicationContext;
}

#include "shield/config/config.hpp"

namespace shield::config {

class Configuration {
public:
    virtual ~Configuration() = default;

    virtual void configure(shield::core::ApplicationContext& context) = 0;

    virtual std::string name() const = 0;

    virtual std::vector<std::string> depends_on() const { return {}; }
};

}  // namespace shield::config

// Include ApplicationContext after defining Configuration interface
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/core/application_context.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/gateway/gateway_service.hpp"
#include "shield/script/lua_vm_pool.hpp"

#ifdef SHIELD_ENABLE_PROMETHEUS
#include "shield/metrics/prometheus_config.hpp"
#include "shield/metrics/prometheus_service.hpp"
#endif

namespace shield::config {

class GatewayConfiguration : public Configuration {
public:
    void configure(shield::core::ApplicationContext& context) override {
        auto& config_manager = ConfigManager::instance();
        auto gateway_config =
            config_manager
                .get_configuration_properties<gateway::GatewayConfig>();

        if (!gateway_config) {
            throw std::runtime_error(
                "GatewayConfig not found. Make sure it's registered.");
        }

        auto actor_system =
            context.get_service<actor::DistributedActorSystem>();
        auto lua_vm_pool = context.get_service<script::LuaVMPool>();

        if (!actor_system || !lua_vm_pool) {
            throw std::runtime_error(
                "Required dependencies not found for GatewayService");
        }

        auto gateway_service = std::make_shared<gateway::GatewayService>(
            "gateway", *actor_system, *lua_vm_pool, gateway_config);

        context.register_service("gateway", gateway_service);
    }

    std::string name() const override { return "GatewayConfiguration"; }

    std::vector<std::string> depends_on() const override {
        return {"ScriptConfiguration", "ActorConfiguration"};
    }
};

class ScriptConfiguration : public Configuration {
public:
    void configure(shield::core::ApplicationContext& context) override {
        script::LuaVMPoolConfig lua_config;
        lua_config.initial_size = 4;
        lua_config.max_size = 16;
        lua_config.min_size = 2;

        auto lua_vm_pool =
            std::make_shared<script::LuaVMPool>("main_pool", lua_config);

        context.register_service("lua_vm_pool", lua_vm_pool);
    }

    std::string name() const override { return "ScriptConfiguration"; }
};

class ApplicationConfiguration : public Configuration {
private:
    std::vector<std::unique_ptr<Configuration>> configurations_;

public:
    ApplicationConfiguration() {
        configurations_.push_back(std::make_unique<ScriptConfiguration>());
#ifdef SHIELD_ENABLE_PROMETHEUS
        configurations_.push_back(std::make_unique<MetricsConfiguration>());
#endif
    }

    void configure(shield::core::ApplicationContext& context) override {
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

#ifdef SHIELD_ENABLE_PROMETHEUS
class MetricsConfiguration : public Configuration {
public:
    void configure(shield::core::ApplicationContext& context) override {
        auto& config_manager = ConfigManager::instance();
        auto prometheus_config =
            config_manager
                .get_configuration_properties<metrics::PrometheusConfig>();

        if (!prometheus_config) {
            prometheus_config = std::make_shared<metrics::PrometheusConfig>();
        }

        auto prometheus_service = std::shared_ptr<metrics::PrometheusService>(
            &metrics::PrometheusService::instance(),
            [](metrics::PrometheusService*) {});

        context.register_service("prometheus", prometheus_service);
    }

    std::string name() const override { return "MetricsConfiguration"; }
};
#endif

}  // namespace shield::config
