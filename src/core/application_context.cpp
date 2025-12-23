#include "shield/core/application_context.hpp"

#include "shield/config/application_configuration.hpp"
#include "shield/config/config.hpp"
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

void ApplicationContext::configure_with_plugins(
    const std::string& plugins_directory) {
    auto plugin_manager = std::make_unique<PluginManager>();
    plugin_manager->add_plugin_directory(plugins_directory);
    configure_with_plugins(std::move(plugin_manager));
}

void ApplicationContext::configure_with_plugins(
    std::unique_ptr<PluginManager> plugin_manager) {
    try {
        SHIELD_LOG_INFO << "Configuring ApplicationContext with Plugin system";

        // Set up event callback
        plugin_manager->set_event_callback([](PluginManager::PluginEvent event,
                                              const std::string& plugin_name,
                                              const std::string& message) {
            switch (event) {
                case PluginManager::PluginEvent::DISCOVERED:
                    SHIELD_LOG_DEBUG << "Plugin discovered: " << plugin_name
                                     << " - " << message;
                    break;
                case PluginManager::PluginEvent::LOADING:
                    SHIELD_LOG_INFO << "Loading plugin: " << plugin_name
                                    << " - " << message;
                    break;
                case PluginManager::PluginEvent::LOADED:
                    SHIELD_LOG_INFO << "Plugin loaded successfully: "
                                    << plugin_name;
                    break;
                case PluginManager::PluginEvent::UNLOADING:
                    SHIELD_LOG_INFO << "Unloading plugin: " << plugin_name;
                    break;
                case PluginManager::PluginEvent::UNLOADED:
                    SHIELD_LOG_INFO << "Plugin unloaded: " << plugin_name;
                    break;
                case PluginManager::PluginEvent::ERROR:
                    SHIELD_LOG_ERROR << "Plugin error: " << plugin_name << " - "
                                     << message;
                    break;
            }
        });

        // Discover and load plugins
        size_t discovered = plugin_manager->discover_plugins();
        SHIELD_LOG_INFO << "Discovered " << discovered << " plugins";

        size_t loaded = plugin_manager->load_all_plugins();
        SHIELD_LOG_INFO << "Loaded " << loaded << " plugins";

        // Manually initialize plugin starters
        auto plugin_starters = plugin_manager->get_plugin_starters();
        for (auto* starter : plugin_starters) {
            if (starter->is_enabled()) {
                try {
                    SHIELD_LOG_INFO << "Initializing plugin starter: "
                                    << starter->name();
                    starter->pre_initialize(*this);
                    starter->initialize(*this);
                    starter->post_initialize(*this);
                    SHIELD_LOG_INFO
                        << "Successfully initialized plugin starter: "
                        << starter->name();
                } catch (const std::exception& e) {
                    SHIELD_LOG_ERROR << "Failed to initialize plugin starter "
                                     << starter->name() << ": " << e.what();
                    throw;
                }
            }
        }

        // Store plugin manager
        plugin_manager_ = std::move(plugin_manager);

        SHIELD_LOG_INFO << "Successfully configured with Plugin system";
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to configure with Plugin system: "
                         << e.what();
        throw;
    }
}

}  // namespace shield::core
