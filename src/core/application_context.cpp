#include "shield/core/application_context.hpp"

#include "shield/config/application_configuration.hpp"
#include "shield/core/starter_manager.hpp"
#include "shield/log/logger.hpp"

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
