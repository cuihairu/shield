#include "shield/core/application_context.hpp"

#include "shield/config/application_configuration.hpp"
#include "shield/config/config.hpp"
#include "shield/core/starter_manager.hpp"
#include "shield/gateway/gateway_config.hpp"
#include "shield/gateway/gateway_service.hpp"
#include "shield/log/logger.hpp"
#include "shield/metrics/prometheus_config.hpp"
#include "shield/metrics/prometheus_service.hpp"

namespace shield::core {

ApplicationContext& ApplicationContext::instance() {
    static ApplicationContext instance;
    return instance;
}

void ApplicationContext::init_all() {
    for (auto& service : m_services_by_order) {
        service->on_init(*this);
    }
}

void ApplicationContext::start_all() {
    for (auto& service : m_services_by_order) {
        service->on_start();
    }
}

void ApplicationContext::stop_all() {
    for (auto it = m_services_by_order.rbegin();
         it != m_services_by_order.rend(); ++it) {
        (*it)->on_stop();
    }
}

void ApplicationContext::subscribe_to_config_reloads(
    const std::shared_ptr<Service>& service) {
    if (auto reloadable =
            std::dynamic_pointer_cast<IReloadableService>(service)) {
        auto& config_manager = config::ConfigManager::instance();

        if (auto gateway =
                std::dynamic_pointer_cast<gateway::GatewayService>(service)) {
            config_manager.subscribe_to_reloads<gateway::GatewayConfig>(
                [weak_service = std::weak_ptr(service)](
                    const gateway::GatewayConfig& new_config) {
                    if (auto locked_service = weak_service.lock()) {
                        if (auto reloadable_service =
                                std::dynamic_pointer_cast<IReloadableService>(
                                    locked_service)) {
                            reloadable_service->on_config_reloaded();
                        }
                    }
                });
        } else if (auto prometheus =
                       std::dynamic_pointer_cast<metrics::PrometheusService>(
                           service)) {
            config_manager.subscribe_to_reloads<metrics::PrometheusConfig>(
                [weak_service = std::weak_ptr(service)](
                    const metrics::PrometheusConfig& new_config) {
                    if (auto locked_service = weak_service.lock()) {
                        if (auto reloadable_service =
                                std::dynamic_pointer_cast<IReloadableService>(
                                    locked_service)) {
                            reloadable_service->on_config_reloaded();
                        }
                    }
                });
        }
        // Add other ReloadableService types here
        // Example for LuaVMPool if it becomes ReloadableService:
        // else if (auto lua_vm_pool =
        // std::dynamic_pointer_cast<script::LuaVMPool>(service)) {
        //     config_manager.subscribe_to_reloads<script::LuaVMPoolConfig>(
        //         [weak_service = std::weak_ptr(service)](const
        //         script::LuaVMPoolConfig& new_config) {
        //             if (auto locked_service = weak_service.lock()) {
        //                 if (auto reloadable_service =
        //                 std::dynamic_pointer_cast<IReloadableService>(locked_service))
        //                 {
        //                     reloadable_service->on_config_reloaded();
        //                 }
        //             }
        //         });
        // }
    }
}

void ApplicationContext::configure_with(
    std::unique_ptr<shield::config::Configuration> configuration) {
    try {
        SHIELD_LOG_INFO << "Configuring ApplicationContext with: "
                        << configuration->name();
        configuration->configure(*this);
        SHIELD_LOG_INFO << "Successfully configured: " << configuration->name();
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to configure " << configuration->name()
                         << ": " << e.what();
        throw;
    }
}

void ApplicationContext::configure_with_starters(
    std::unique_ptr<StarterManager> starter_manager) {
    try {
        SHIELD_LOG_INFO << "Configuring ApplicationContext with Starter system";
        starter_manager->initialize_all(*this);
        SHIELD_LOG_INFO << "Successfully configured with Starter system";
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to configure with Starter system: "
                         << e.what();
        throw;
    }
}

}  // namespace shield::core